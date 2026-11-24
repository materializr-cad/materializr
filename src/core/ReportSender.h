#pragma once
// Posts a bug report to the Materializr relay (a small Cloudflare Worker that
// holds the GitHub App credentials and files the issue). One blocking call; run
// it off the UI thread.
//
//   desktop  libcurl multipart POST
//   Android  JNI into MaterializrActivity.nativePostReport (HttpURLConnection) -
//            the mobile build has no libcurl
//   iOS      NSURLSession upload (ios_report.mm)
//
// Nothing here talks to GitHub directly and the app carries no credentials.

#include "../platform_defs.h"

#include <string>
#include <vector>

namespace materializr {

struct ReportField { std::string name, value; };
struct ReportFile  { std::string name, path, contentType; };

struct ReportSendResult {
    bool ok = false;
    int httpStatus = 0;        // 0 = never got an HTTP answer
    std::string issueUrl;      // set when ok
    std::string message;       // human-readable; the relay's own text when it sent one
};

bool reportSenderAvailable();

// Blocking. `url` must be https. Files that can't be read make the call fail
// with a message rather than sending a partial report.
ReportSendResult sendReport(const std::string& url,
                            const std::vector<ReportField>& fields,
                            const std::vector<ReportFile>& files);

// Turns the relay's JSON reply (any HTTP status) into a result. Pure, so it is
// unit-tested; both platform paths use it.
ReportSendResult parseRelayReply(int httpStatus, const std::string& body);

#if defined(__ANDROID__)
// Implemented in android_files.cpp. Returns "HTTP|<status>|<body>" or "ERR|<message>".
std::string androidPostReport(const std::string& url, const std::string& spec);
#elif defined(MZ_IOS)
// Implemented in ios_report.mm. True when an HTTP answer came back (any status),
// with `httpStatus` and `body` set; false with `error` set otherwise.
bool iosPostReport(const std::string& url, const std::vector<ReportField>& fields,
                   const std::vector<ReportFile>& files, int& httpStatus, std::string& body,
                   std::string& error);
#endif

} // namespace materializr
