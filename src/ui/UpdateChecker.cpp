#include "UpdateChecker.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

#ifndef MATERIALIZR_VERSION
#define MATERIALIZR_VERSION "0.0.0"
#endif

namespace materializr {

namespace {

// libcurl write callback: append into a std::string.
size_t writeToString(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    std::string* out = static_cast<std::string*>(userp);
    // Response-size cap: the GitHub release JSON is a few KB. Refuse to grow past
    // this so a hostile/huge response can't exhaust memory. Returning less than
    // `total` makes libcurl abort the transfer with CURLE_WRITE_ERROR.
    const size_t kMaxResponse = 4u * 1024 * 1024;
    if (out->size() + total > kMaxResponse) return 0;
    out->append(static_cast<char*>(contents), total);
    return total;
}

// Extract a quoted JSON string value for a top-level "key". Skips whitespace
// after the colon and reads through escapes minimally - sufficient for the few
// fields we want (tag_name, html_url) without dragging in a JSON dependency.
std::string findJsonString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return {};
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return {};
    ++p;
    while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
    if (p >= json.size() || json[p] != '"') return {};
    ++p;
    std::string out;
    while (p < json.size() && json[p] != '"') {
        if (json[p] == '\\' && p + 1 < json.size()) {
            char c = json[p + 1];
            if (c == '/') out += '/';
            else if (c == 'n') out += '\n';
            else if (c == 't') out += '\t';
            else if (c == '"') out += '"';
            else if (c == '\\') out += '\\';
            else out += c;
            p += 2;
        } else {
            out += json[p++];
        }
    }
    return out;
}

// Strip a leading "v"/"V" - release tags are usually "v0.1.0".
std::string stripV(std::string s) {
    if (!s.empty() && (s.front() == 'v' || s.front() == 'V')) s.erase(0, 1);
    return s;
}

std::vector<int> parseNumericComponents(const std::string& v) {
    std::vector<int> parts;
    std::stringstream ss(v);
    std::string tok;
    while (std::getline(ss, tok, '.')) {
        // Drop any non-digit suffix (e.g. "1-rc2" -> 1).
        size_t i = 0;
        while (i < tok.size() && std::isdigit(static_cast<unsigned char>(tok[i]))) ++i;
        if (i == 0) parts.push_back(0);
        else {
            // A hostile/oversized version token (e.g. a crafted tag_name with a
            // 40-digit number) would make std::stoi throw out_of_range and crash
            // the app on an uncaught exception - treat as 0 instead.
            try { parts.push_back(std::stoi(tok.substr(0, i))); }
            catch (...) { parts.push_back(0); }
        }
    }
    return parts;
}

bool isAllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

// Compare two semver pre-release tag lists (the dot-separated bits after the
// "-"). An EMPTY list means a final release, which outranks any pre-release.
// Otherwise compare identifier by identifier: numeric ones numerically, a
// numeric identifier ranks below a non-numeric one, and a shorter prefix ranks
// below a longer one (1.0-beta < 1.0-beta.1). Mirrors semver §11.
int comparePre(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if (a.empty() && b.empty()) return 0;
    if (a.empty()) return 1;   // a is a final release -> higher
    if (b.empty()) return -1;
    size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        if (i >= a.size()) return -1; // a is a shorter prefix of b
        if (i >= b.size()) return 1;
        const std::string& x = a[i];
        const std::string& y = b[i];
        bool xn = isAllDigits(x), yn = isAllDigits(y);
        if (xn && yn) {
            int xi = 0, yi = 0;
            try { xi = std::stoi(x); } catch (...) {}
            try { yi = std::stoi(y); } catch (...) {}
            if (xi != yi) return xi < yi ? -1 : 1;
        } else if (xn != yn) {
            return xn ? -1 : 1;       // numeric identifiers rank below alphanumeric
        } else {
            int c = x.compare(y);
            if (c != 0) return c < 0 ? -1 : 1;
        }
    }
    return 0;
}

} // namespace

int UpdateChecker::compareVersions(const std::string& a, const std::string& b) {
    // Split "X.Y.Z-pre.tokens" into numeric core + pre-release token list.
    auto split = [](std::string v) {
        v = stripV(v);
        std::string core = v, pre;
        size_t dash = v.find('-');
        if (dash != std::string::npos) { core = v.substr(0, dash); pre = v.substr(dash + 1); }
        std::vector<std::string> preToks;
        if (!pre.empty()) {
            std::stringstream ss(pre);
            std::string tok;
            while (std::getline(ss, tok, '.')) preToks.push_back(tok);
        }
        return std::make_pair(parseNumericComponents(core), preToks);
    };
    auto [ca, pra] = split(a);
    auto [cb, prb] = split(b);
    size_t n = std::max(ca.size(), cb.size());
    ca.resize(n, 0);
    cb.resize(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (ca[i] < cb[i]) return -1;
        if (ca[i] > cb[i]) return  1;
    }
    return comparePre(pra, prb);
}

UpdateChecker::Result UpdateChecker::check(const std::string& owner,
                                           const std::string& repo,
                                           bool includePrereleases) {
    Result r;
    r.current = MATERIALIZR_VERSION;
    r.releasePageUrl = "https://github.com/" + owner + "/" + repo + "/releases";

    CURL* curl = curl_easy_init();
    if (!curl) { r.errorMessage = "Failed to initialise libcurl."; return r; }

    // Stable channel uses /releases/latest (GitHub excludes pre-releases from
    // it). Beta channel uses /releases, which lists everything newest-first -
    // the first element is the most recent build, pre-release or not. Both
    // responses put the release's own tag_name / html_url first in the JSON
    // (before the nested author/asset objects), so the same first-match parse
    // works for either.
    std::string url = "https://api.github.com/repos/" + owner + "/" + repo +
                      (includePrereleases ? "/releases?per_page=10"
                                          : "/releases/latest");
    std::string body;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    headers = curl_slist_append(headers, "User-Agent: Materializr-UpdateChecker");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    // Pin the request and any redirect target to HTTPS, so a redirect can't
    // downgrade to http or jump to file://, etc. The *_STR forms are libcurl
    // 7.85+; fall back to the numeric protocol bitmask on older libcurl.
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

    CURLcode code = curl_easy_perform(curl);
    long httpStatus = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        r.errorMessage = curl_easy_strerror(code);
        return r;
    }
    if (httpStatus == 404) {
        // No releases published yet - treat as "up to date" rather than an error.
        r.ok = true;
        r.latest = r.current;
        r.errorMessage = "No releases published yet.";
        return r;
    }
    if (httpStatus < 200 || httpStatus >= 300) {
        r.errorMessage = "GitHub returned HTTP " + std::to_string(httpStatus);
        return r;
    }

    std::string tag = findJsonString(body, "tag_name");
    if (tag.empty()) {
        // Beta channel with an empty release list ("[]") lands here - treat it
        // as "nothing newer" rather than an error.
        if (includePrereleases) {
            r.ok = true;
            r.latest = r.current;
            r.errorMessage = "No pre-release builds published yet.";
            return r;
        }
        r.errorMessage = "Could not find tag_name in GitHub response.";
        return r;
    }
    std::string pageUrl = findJsonString(body, "html_url");
    if (!pageUrl.empty()) r.releasePageUrl = pageUrl;

    r.ok = true;
    r.latest = stripV(tag);
    r.updateAvailable = compareVersions(r.current, r.latest) < 0;
    return r;
}

} // namespace materializr
