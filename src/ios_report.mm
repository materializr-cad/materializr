// iOS half of core/ReportSender: POSTs a bug report to the relay with
// NSURLSession. The counterpart of MaterializrActivity.nativePostReport on
// Android, and sends the same multipart body.
//
// iOS needs no permission or entitlement for outgoing HTTPS (App Transport
// Security only blocks plain http, and the relay is https).
//
// The body goes to a temp file first and is uploaded from there: the relay
// refuses chunked uploads, an upload from a file always carries
// Content-Length, and a large project attachment never sits in memory.
#include "core/ReportSender.h"

#if defined(MZ_IOS)

#import <Foundation/Foundation.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

// The relay never redirects. Refusing to follow one hands the 3xx back to
// parseRelayReply, which turns it into a "couldn't be sent" message.
@interface MZReportSessionDelegate : NSObject <NSURLSessionTaskDelegate>
@end

@implementation MZReportSessionDelegate
- (void)URLSession:(NSURLSession*)session
                          task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
                    newRequest:(NSURLRequest*)request
             completionHandler:(void (^)(NSURLRequest*))completionHandler {
    completionHandler(nil);
}
@end

namespace materializr {

namespace {

// Writes the multipart body: the same layout as the Android sender.
bool writeMultipart(const std::string& path, const std::string& boundary,
                    const std::vector<ReportField>& fields, const std::vector<ReportFile>& files,
                    std::string& error) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { error = "Couldn't prepare the report for sending."; return false; }
    for (const auto& f : fields) {
        out << "--" << boundary << "\r\nContent-Disposition: form-data; name=\"" << f.name
            << "\"\r\n\r\n";
        out.write(f.value.data(), static_cast<std::streamsize>(f.value.size()));
        out << "\r\n";
    }
    for (const auto& f : files) {
        std::ifstream in(f.path, std::ios::binary);
        if (!in) { error = "Couldn't read " + f.name + " to attach it."; return false; }
        out << "--" << boundary << "\r\nContent-Disposition: form-data; name=\"" << f.name
            << "\"; filename=\"" << f.name << "\"\r\n";
        if (!f.contentType.empty()) out << "Content-Type: " << f.contentType << "\r\n";
        out << "\r\n" << in.rdbuf() << "\r\n";
    }
    out << "--" << boundary << "--\r\n";
    out.flush();
    if (!out) { error = "Couldn't prepare the report for sending."; return false; }
    return true;
}

} // namespace

bool iosPostReport(const std::string& url, const std::vector<ReportField>& fields,
                   const std::vector<ReportFile>& files, int& httpStatus, std::string& body,
                   std::string& error) {
    httpStatus = 0;
    body.clear();
    error.clear();
    if (url.rfind("https://", 0) != 0) { error = "The report service address is invalid."; return false; }

    @autoreleasepool {
        NSURL* nsUrl = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
        if (!nsUrl) { error = "The report service address is invalid."; return false; }

        NSString* uuid = [NSUUID UUID].UUIDString;
        const std::string boundary = std::string("----materializr") + uuid.UTF8String;
        NSString* tmp = [NSTemporaryDirectory()
            stringByAppendingPathComponent:[NSString stringWithFormat:@"materializr-report-%@.body", uuid]];
        const std::string tmpPath = tmp.fileSystemRepresentation;

        if (!writeMultipart(tmpPath, boundary, fields, files, error)) {
            [[NSFileManager defaultManager] removeItemAtPath:tmp error:nil];
            return false;
        }

        NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:nsUrl];
        req.HTTPMethod = @"POST";
        [req setValue:[NSString stringWithFormat:@"multipart/form-data; boundary=%s", boundary.c_str()]
            forHTTPHeaderField:@"Content-Type"];
        [req setValue:@"Materializr-report/1 (iOS)" forHTTPHeaderField:@"User-Agent"];

        // Same limits as the other platforms: 90 s without progress fails the
        // request. The overall cap leaves room for a big project on a slow link.
        NSURLSessionConfiguration* cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        cfg.timeoutIntervalForRequest = 90;
        cfg.timeoutIntervalForResource = 300;
        cfg.HTTPCookieStorage = nil;
        cfg.URLCache = nil;
        NSURLSession* session = [NSURLSession sessionWithConfiguration:cfg
                                                              delegate:[MZReportSessionDelegate new]
                                                         delegateQueue:nil];

        // Blocking by contract (the dialog calls this from a worker thread);
        // the completion handler runs on the session's own queue.
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block NSInteger status = 0;
        __block NSData* replyData = nil;
        __block NSError* replyError = nil;
        NSURLSessionUploadTask* task =
            [session uploadTaskWithRequest:req
                                  fromFile:[NSURL fileURLWithPath:tmp]
                         completionHandler:^(NSData* data, NSURLResponse* response, NSError* err) {
                             if ([response isKindOfClass:[NSHTTPURLResponse class]])
                                 status = ((NSHTTPURLResponse*)response).statusCode;
                             replyData = data;
                             replyError = err;
                             dispatch_semaphore_signal(done);
                         }];
        [task resume];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        [session finishTasksAndInvalidate];   // releases the delegate
        [[NSFileManager defaultManager] removeItemAtPath:tmp error:nil];

        if (status == 0) {
            NSString* why = replyError.localizedDescription;
            error = std::string("Couldn't reach the report service") +
                    (why.length ? std::string(" (") + why.UTF8String + ")" : std::string()) + ".";
            return false;
        }
        httpStatus = static_cast<int>(status);
        if (replyData.length) {
            const size_t n = std::min<size_t>(replyData.length, 64 * 1024);   // the relay's replies are tiny
            body.assign(static_cast<const char*>(replyData.bytes), n);
        }
        return true;
    }
}

} // namespace materializr

#endif // MZ_IOS
