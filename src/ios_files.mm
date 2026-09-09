// iOS implementation of the mobile_files.h picker/share bridge, mirroring the
// Android SAF flow with UIKit equivalents:
//
//   open  - UIDocumentPickerViewController (open-in-place), then the picked
//           document is copied to an app temp path (NSFileCoordinator handles
//           iCloud downloads) and that path is delivered via the poll, exactly
//           like Android's copy-through-ContentResolver.
//   save  - iOS's export picker needs an EXISTING file, the reverse of SAF's
//           pick-destination-first. So mobileStartCreateDocument() completes
//           immediately with "ok" (the caller then writes the temp file), and
//           mobileCommitSave() presents the export sheet for the just-written
//           temp. Consequence: a cancelled export leaves the app believing the
//           save succeeded, and the recents ref recorded right after a save is
//           the temp path (self-heals: Open Recent drops it on first failure).
//           TODO(phase-4): make the app-side save flow await the export result.
//   recents - security-scoped bookmarks (base64 in the ref string) stand in
//           for Android's persistable content:// URIs.
//
// All calls arrive on the main thread (SDL's uikit main IS the UIKit main
// thread), so presenting view controllers directly is safe.
#include "mobile_files.h"

#if defined(MZ_IOS)

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cstdio>
#include <string>

namespace {

enum class PickMode { None, Open, Export };

struct {
    PickMode mode = PickMode::None;
    bool ready = false;
    std::string value;      // temp path (open) / "ok" (save) / "" (cancelled)
    std::string lastUri;    // base64 security-scoped bookmark of the last pick
    std::string lastName;   // display name of the last pick
} g_pick;

std::string tempPathFor(NSString* name) {
    NSString* dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"mz_docs"];
    [NSFileManager.defaultManager createDirectoryAtPath:dir
                            withIntermediateDirectories:YES attributes:nil error:nil];
    NSString* base = name.lastPathComponent;
    if (base.length == 0) base = @"file.bin";
    return std::string([dir stringByAppendingPathComponent:base].UTF8String);
}

// Copy a (possibly iCloud / provider-backed) document to a local temp path.
// NSFileCoordinator triggers the download and blocks until readable.
bool copyToTemp(NSURL* src, const std::string& destPath) {
    NSFileCoordinator* fc = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    NSError* coordErr = nil;
    __block BOOL ok = NO;
    [fc coordinateReadingItemAtURL:src
                           options:NSFileCoordinatorReadingWithoutChanges
                             error:&coordErr
                        byAccessor:^(NSURL* readURL) {
        NSFileManager* fm = NSFileManager.defaultManager;
        NSURL* dst = [NSURL fileURLWithPath:@(destPath.c_str())];
        [fm removeItemAtURL:dst error:nil];
        NSError* cpErr = nil;
        ok = [fm copyItemAtURL:readURL toURL:dst error:&cpErr];
        if (!ok)
            std::fprintf(stderr, "ios_files: copy failed: %s\n",
                         cpErr.localizedDescription.UTF8String);
    }];
    return ok && !coordErr;
}

// Record the bookmark + display name of a picked/exported document so the
// "Open Recent" list gets a ref that survives relaunches.
void recordLastDoc(NSURL* url) {
    NSError* err = nil;
    NSData* bm = [url bookmarkDataWithOptions:0
               includingResourceValuesForKeys:nil
                                relativeToURL:nil
                                        error:&err];
    g_pick.lastUri  = bm ? std::string([bm base64EncodedStringWithOptions:0].UTF8String) : "";
    g_pick.lastName = url.lastPathComponent ? url.lastPathComponent.UTF8String : "";
}

// The SDL uikit window is a plain UIWindow; walk to whatever is frontmost so
// pickers/share sheets present even if something else is already up.
UIViewController* topViewController() {
    UIWindow* win = nil;
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        for (UIWindow* w in ((UIWindowScene*)scene).windows)
            if (w.isKeyWindow) { win = w; break; }
        if (win) break;
    }
    if (!win) win = UIApplication.sharedApplication.windows.firstObject;
    UIViewController* vc = win.rootViewController;
    while (vc.presentedViewController) vc = vc.presentedViewController;
    return vc;
}

} // namespace

@interface MZDocPickerDelegate : NSObject <UIDocumentPickerDelegate>
@end

@implementation MZDocPickerDelegate
- (void)documentPicker:(UIDocumentPickerViewController*)controller
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    NSURL* url = urls.firstObject;
    if (!url) { [self documentPickerWasCancelled:controller]; return; }

    if (g_pick.mode == PickMode::Open) {
        BOOL scoped = [url startAccessingSecurityScopedResource];
        recordLastDoc(url);
        std::string tmp = tempPathFor(url.lastPathComponent);
        bool ok = copyToTemp(url, tmp);
        if (scoped) [url stopAccessingSecurityScopedResource];
        g_pick.value = ok ? tmp : "";
        g_pick.ready = true;
    } else {
        // Export completed: the document now lives at the user's destination.
        // Nothing is polled for at this point (see header note) - just record
        // the destination for later mobileLastDocUri() queries.
        BOOL scoped = [url startAccessingSecurityScopedResource];
        recordLastDoc(url);
        if (scoped) [url stopAccessingSecurityScopedResource];
        g_pick.mode = PickMode::None;
    }
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
    if (g_pick.mode == PickMode::Open) {
        g_pick.value = "";
        g_pick.ready = true;
    } else {
        std::fprintf(stderr, "ios_files: export cancelled - file not saved\n");
        g_pick.mode = PickMode::None;
    }
}
@end

static MZDocPickerDelegate* pickerDelegate() {
    static MZDocPickerDelegate* d = [MZDocPickerDelegate new];
    return d;
}

namespace materializr {

bool mobileStartOpenDocument(const std::string& /*mimeCsv*/) {
    UIViewController* top = topViewController();
    if (!top) return false;
    // "*/*" on Android; same idea here - show everything, the user decides.
    UIDocumentPickerViewController* picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeItem ]
                                                                    asCopy:NO];
    picker.delegate = pickerDelegate();
    g_pick.mode = PickMode::Open;
    g_pick.ready = false;
    [top presentViewController:picker animated:YES completion:nil];
    return true;
}

bool mobileStartCreateDocument(const std::string& /*suggestedName*/,
                               const std::string& /*mime*/) {
    // Destination is chosen AFTER the write on iOS (export model) - complete
    // the "picker" immediately so the caller writes its temp file, then
    // mobileCommitSave() presents the real export sheet.
    g_pick.mode = PickMode::None;
    g_pick.value = "ok";
    g_pick.ready = true;
    return true;
}

bool mobilePollFileResult(std::string& outValue) {
    if (!g_pick.ready) return false;
    outValue = g_pick.value;
    g_pick.ready = false;
    g_pick.value.clear();
    if (g_pick.mode == PickMode::Open) g_pick.mode = PickMode::None;
    return true;
}

bool mobileCommitSave(const std::string& tempPath) {
    UIViewController* top = topViewController();
    if (!top) return false;
    NSURL* tmp = [NSURL fileURLWithPath:@(tempPath.c_str())];
    if (![NSFileManager.defaultManager fileExistsAtPath:tmp.path]) {
        std::fprintf(stderr, "ios_files: commitSave: temp missing: %s\n", tempPath.c_str());
        return false;
    }
    UIDocumentPickerViewController* picker =
        [[UIDocumentPickerViewController alloc] initForExportingURLs:@[ tmp ] asCopy:YES];
    picker.delegate = pickerDelegate();
    g_pick.mode = PickMode::Export;
    [top presentViewController:picker animated:YES completion:nil];
    return true; // optimistic - see header note about cancelled exports
}

void mobileShareFile(const std::string& path, const std::string& /*mime*/) {
    UIViewController* top = topViewController();
    if (!top) return;
    NSURL* url = [NSURL fileURLWithPath:@(path.c_str())];
    UIActivityViewController* sheet =
        [[UIActivityViewController alloc] initWithActivityItems:@[ url ]
                                          applicationActivities:nil];
    // iPad requires a popover anchor or UIKit throws.
    sheet.popoverPresentationController.sourceView = top.view;
    sheet.popoverPresentationController.sourceRect =
        CGRectMake(CGRectGetMidX(top.view.bounds), CGRectGetMidY(top.view.bounds), 1, 1);
    sheet.popoverPresentationController.permittedArrowDirections = 0;
    [top presentViewController:sheet animated:YES completion:nil];
}

bool mobileCommitSaveToRef(const std::string& /*ref*/, const std::string& /*tempPath*/) {
    // Not wired yet on iOS: quick-save falls back to the Save As picker,
    // which is today's shipping behaviour. (Implementable with the stored
    // security-scoped bookmark + startAccessingSecurityScopedResource.)
    return false;
}

std::string mobileLastDocUri()  { return g_pick.lastUri; }
std::string mobileLastDocName() { return g_pick.lastName; }

std::string mobileOpenUri(const std::string& uri) {
    NSData* bm = [[NSData alloc] initWithBase64EncodedString:@(uri.c_str()) options:0];
    if (!bm) return {};
    BOOL stale = NO;
    NSError* err = nil;
    NSURL* url = [NSURL URLByResolvingBookmarkData:bm
                                           options:NSURLBookmarkResolutionWithoutUI
                                     relativeToURL:nil
                               bookmarkDataIsStale:&stale
                                             error:&err];
    if (!url) return {};
    BOOL scoped = [url startAccessingSecurityScopedResource];
    if (stale) recordLastDoc(url); // refresh the stored ref on next save of recents
    std::string tmp = tempPathFor(url.lastPathComponent);
    bool ok = copyToTemp(url, tmp);
    if (scoped) [url stopAccessingSecurityScopedResource];
    return ok ? tmp : std::string{};
}

// mobileOpenUri() above never substitutes a backup copy - it returns the real
// document or fails - so an open here is never "via fallback".
bool mobileLastOpenWasFallback() { return false; }

// SDL's iOS backend raises/dismisses the system keyboard itself from
// SDL_StartTextInput/SDL_StopTextInput - the Android IME workaround isn't
// needed here.
void mobileShowTextInput() {}
void mobileHideTextInput() {}

} // namespace materializr

#endif // MZ_IOS
