#include "ReportSender.h"

#include "../platform_defs.h"
#include "Base64.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#if !defined(MZ_MOBILE)
#include <curl/curl.h>
#endif

namespace materializr {

namespace {

// The relay's replies are small flat JSON objects it writes itself, so a minimal
// reader is enough (and keeps the mobile builds free of a JSON dependency).
bool jsonField(const std::string& j, const std::string& key, size_t& valuePos) {
    const std::string needle = "\"" + key + "\"";
    size_t p = j.find(needle);
    if (p == std::string::npos) return false;
    p = j.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < j.size() && std::isspace(static_cast<unsigned char>(j[p]))) ++p;
    valuePos = p;
    return p < j.size();
}

std::string jsonString(const std::string& j, const std::string& key) {
    size_t p;
    if (!jsonField(j, key, p) || j[p] != '"') return {};
    ++p;
    std::string out;
    while (p < j.size() && j[p] != '"') {
        if (j[p] == '\\' && p + 1 < j.size()) {
            const char c = j[p + 1];
            out += (c == 'n') ? '\n' : (c == 't') ? '\t' : c;   // \" \\ \/ pass through as themselves
            p += 2;
        } else {
            out += j[p++];
        }
    }
    return out;
}

bool jsonTrue(const std::string& j, const std::string& key) {
    size_t p;
    return jsonField(j, key, p) && j.compare(p, 4, "true") == 0;
}

} // namespace

ReportSendResult parseRelayReply(int httpStatus, const std::string& body) {
    ReportSendResult r;
    r.httpStatus = httpStatus;
    const std::string url = jsonString(body, "issueUrl");
    if (httpStatus == 200 && jsonTrue(body, "ok") && url.rfind("https://", 0) == 0) {
        r.ok = true;
        r.issueUrl = url;
        return r;
    }
    const std::string msg = jsonString(body, "message");
    if (!msg.empty()) {
        r.message = msg;
    } else if (httpStatus == 429) {
        r.message = "You've sent several reports recently. Please try again later.";
    } else if (httpStatus >= 500) {
        r.message = "The report service is having trouble right now. Please try again later.";
    } else {
        r.message = "The report couldn't be sent (HTTP " + std::to_string(httpStatus) + ").";
    }
    return r;
}

#if defined(__ANDROID__)

bool reportSenderAvailable() { return true; }

ReportSendResult sendReport(const std::string& url, const std::vector<ReportField>& fields,
                            const std::vector<ReportFile>& files) {
    // Line protocol to Java: F|name|base64(value)  and  P|name|path|contentType.
    // Values are base64 so newlines and unicode in the description survive.
    std::string spec;
    for (const auto& f : fields)
        spec += "F|" + f.name + "|" +
                base64Encode(reinterpret_cast<const uint8_t*>(f.value.data()), f.value.size()) + "\n";
    for (const auto& f : files) spec += "P|" + f.name + "|" + f.path + "|" + f.contentType + "\n";

    const std::string out = androidPostReport(url, spec);
    ReportSendResult r;
    if (out.rfind("HTTP|", 0) == 0) {
        const size_t p = out.find('|', 5);
        if (p != std::string::npos)
            return parseRelayReply(std::atoi(out.substr(5, p - 5).c_str()), out.substr(p + 1));
    }
    r.message = out.rfind("ERR|", 0) == 0 ? out.substr(4) : "Couldn't reach the report service.";
    return r;
}

#elif defined(MZ_MOBILE)   // iOS: no networking build

bool reportSenderAvailable() { return false; }

ReportSendResult sendReport(const std::string&, const std::vector<ReportField>&,
                            const std::vector<ReportFile>&) {
    ReportSendResult r;
    r.message = "Sending reports from inside the app isn't available on this platform.";
    return r;
}

#else   // desktop: libcurl

namespace {

size_t collect(void* data, size_t size, size_t nmemb, void* userp) {
    auto* out = static_cast<std::string*>(userp);
    const size_t n = size * nmemb;
    if (out->size() + n > 64 * 1024) return 0;   // the relay's replies are tiny
    out->append(static_cast<char*>(data), n);
    return n;
}

} // namespace

bool reportSenderAvailable() { return true; }

ReportSendResult sendReport(const std::string& url, const std::vector<ReportField>& fields,
                            const std::vector<ReportFile>& files) {
    ReportSendResult r;
    if (url.rfind("https://", 0) != 0) { r.message = "The report service address is invalid."; return r; }

    CURL* curl = curl_easy_init();
    if (!curl) { r.message = "Couldn't start the network library."; return r; }
    curl_mime* mime = curl_mime_init(curl);

    for (const auto& f : fields) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, f.name.c_str());
        curl_mime_data(part, f.value.data(), f.value.size());
    }
    for (const auto& f : files) {
        std::ifstream probe(f.path, std::ios::binary);
        if (!probe) {
            curl_mime_free(mime);
            curl_easy_cleanup(curl);
            r.message = "Couldn't read " + f.name + " to attach it.";
            return r;
        }
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, f.name.c_str());
        curl_mime_filedata(part, f.path.c_str());
        curl_mime_filename(part, f.name.c_str());
        if (!f.contentType.empty()) curl_mime_type(part, f.contentType.c_str());
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);          // the relay never redirects
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Materializr-report/1");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);                // we run on a worker thread

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        r.message = std::string("Couldn't reach the report service (") + curl_easy_strerror(rc) + ").";
        return r;
    }
    return parseRelayReply(static_cast<int>(status), response);
}

#endif

} // namespace materializr
