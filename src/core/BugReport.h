#pragma once
// "Report a bug": builds the pre-filled GitHub issue the app hands to the
// user's browser. Pure string work, no ImGui / GL / network, so it is testable
// headless. Nothing here talks to GitHub: the user's own browser (and login)
// does the posting, which is why the app needs no token, no server, and no
// network permission on mobile.

#include <cstddef>
#include <cstdio>
#include <string>

namespace materializr {
namespace bugreport {

// What we know about the machine and the app. Every field is plain text so the
// dialog can show the user exactly what will be included.
struct Env {
    std::string version;     // MATERIALIZR_VERSION
    std::string os;          // "Linux", "Windows", "macOS", "Android", "iOS"
    std::string arch;        // "x86_64", "arm64"
    std::string occt;        // OpenCASCADE version
    std::string graphics;    // GL_RENDERER string
    std::string layout;      // Classic / Modern / ImTouch
    std::string touchMode;   // "on" / "off"
    std::string units;       // display unit
    std::string language;    // UI language
};

// RFC 3986 percent-encoding of every byte outside the unreserved set.
inline std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                          c == '.' || c == '~';
        if (keep) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

inline std::string envBlock(const Env& e) {
    std::string b;
    auto line = [&](const char* k, const std::string& v) {
        if (!v.empty()) b += std::string("- ") + k + ": " + v + "\n";
    };
    line("Materializr", e.version);
    line("OS", e.os + (e.arch.empty() ? "" : " (" + e.arch + ")"));
    line("OpenCASCADE", e.occt);
    line("Graphics", e.graphics);
    line("Layout", e.layout);
    line("Touch mode", e.touchMode);
    line("Units", e.units);
    line("Language", e.language);
    return b;
}

// Minimal JSON string escape (control chars, quote, backslash).
inline std::string jsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += static_cast<char>(c);
        }
    }
    return o;
}

// The environment as the JSON object the relay expects (keys match its allow-list).
inline std::string envJson(const Env& e) {
    std::string j = "{";
    bool first = true;
    auto add = [&](const char* k, const std::string& v) {
        if (v.empty()) return;
        if (!first) j += ",";
        first = false;
        j += std::string("\"") + k + "\":\"" + jsonEscape(v) + "\"";
    };
    add("version", e.version);
    add("os", e.os);
    add("arch", e.arch);
    add("occt", e.occt);
    add("graphics", e.graphics);
    add("layout", e.layout);
    add("touchMode", e.touchMode);
    add("units", e.units);
    add("language", e.language);
    return j + "}";
}

// The issue body. `attachments` lists file names the reporter is asked to drag
// into the comment box (GitHub has no API for attaching files, so the browser
// page is where they go); empty = nothing to attach.
inline std::string buildBody(const Env& e, const std::string& description,
                             const std::string& attachments,
                             const std::string& kind = std::string()) {
    std::string body;
    if (!kind.empty()) body += "**Type:** " + kind + "\n\n";
    body += "### What happened\n\n";
    body += description.empty() ? std::string("(please describe what you were doing)")
                                : description;
    body += "\n\n";
    if (!attachments.empty()) {
        body += "### Attachments\n\n";
        body += "Drag these files into this box:\n\n" + attachments + "\n";
    }
    body += "### Environment\n\n";
    body += envBlock(e);
    body += "\n_Reported from inside Materializr._\n";
    return body;
}

// Never cut a UTF-8 sequence in half.
inline std::string truncateUtf8(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t n = maxBytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}

// GitHub accepts roughly 8 KB of query string before it refuses the request,
// and browsers/OS URL handlers have their own limits, so the whole URL is held
// to `maxUrlLen`. A long description is cut (and says so) rather than the link
// silently failing to open.
inline std::string buildIssueUrl(const std::string& repoIssuesNew,
                                 const std::string& title,
                                 const std::string& body,
                                 size_t maxUrlLen = 6000,
                                 const std::string& label = std::string()) {
    const std::string t = title.empty() ? std::string("Bug report") : title;
    // `labels` is honoured only for people with triage rights on the repo; for
    // everyone else GitHub quietly drops it, so it is harmless to always send.
    std::string head = repoIssuesNew + "?title=" + urlEncode(t);
    if (!label.empty()) head += "&labels=" + urlEncode(label);
    head += "&body=";
    std::string b = body;
    std::string url = head + urlEncode(b);
    if (url.size() <= maxUrlLen) return url;

    const std::string note = "\n\n_(text shortened to fit the link - paste the rest here)_";
    // Percent-encoding triples non-ASCII bytes, so shrink geometrically until
    // it fits instead of guessing a ratio.
    size_t keep = b.size();
    while (keep > 0) {
        keep = keep * 9 / 10;
        b = truncateUtf8(body, keep) + note;
        url = head + urlEncode(b);
        if (url.size() <= maxUrlLen) break;
    }
    return url;
}

// "materializr-bug-20261117-142530" - sorts by time, no characters a file
// manager or GitHub's uploader would choke on.
inline std::string fileStem(int year, int mon, int day, int hour, int min, int sec) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "materializr-bug-%04d%02d%02d-%02d%02d%02d",
                  year, mon, day, hour, min, sec);
    return buf;
}

} // namespace bugreport
} // namespace materializr
