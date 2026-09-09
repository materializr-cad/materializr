#pragma once
#include "platform_defs.h"
#include <string>

// Mobile system file-picker / share bridge. One platform-neutral API with an
// implementation per OS:
//   Android - android_files.cpp (JNI into MaterializrActivity: Storage Access
//             Framework pickers, FileProvider share sheet, persistable URIs).
//   iOS     - ios_files.mm (UIDocumentPickerViewController, UIActivityViewController
//             share sheet, security-scoped bookmarks for recents).
// The async contract is identical on both: start a picker, poll each frame.
// On desktop these are inline no-ops, so FileDialogs.cpp can reference them and
// the desktop file picker (portable-file-dialogs) is untouched.
namespace materializr {

#if defined(MZ_MOBILE)

// Launch the system open / "save as" pickers. mimeCsv is comma-separated MIME
// types (or "*/*"). The result is delivered asynchronously - poll with
// mobilePollFileResult() each frame. Returns false if the host UI is missing.
bool mobileStartOpenDocument(const std::string& mimeCsv);
bool mobileStartCreateDocument(const std::string& suggestedName, const std::string& mime);

// Poll the pending picker. Returns true once a result is in: `outValue` is then
// the temp path to read (open), or "ok" (save: write your temp, then commit), or
// "" if the user cancelled. Returns false while still pending.
bool mobilePollFileResult(std::string& outValue);

// After an open picker the chosen document has already been copied to a temp
// path. After a save picker, write your data to a temp file and call this to copy
// it into the user's chosen destination. Returns true on success.
bool mobileCommitSave(const std::string& tempPath);

// Overwrite an already-picked document ref IN PLACE (quick-save, no picker).
// Android: content:// URI with a persisted write grant. iOS: not implemented
// yet (returns false - callers fall back to the Save As picker), so iOS
// behaviour is unchanged while it's under App Store review/testing.
bool mobileCommitSaveToRef(const std::string& ref, const std::string& tempPath);

// Hand a just-written file to the system share sheet.
void mobileShareFile(const std::string& path, const std::string& mime);

// Persisted-ref support for the "Open Recent" list. After a successful open or
// save picker, the platform layer holds a *persistable* reference to the chosen
// document (Android: persistable-permission content:// URI; iOS: base64
// security-scoped bookmark); these return that ref and its display name so it
// can be stored as a recent. mobileOpenUri() re-opens such a stored ref without
// a picker - it copies the document to a cache temp file and returns that path
// (empty on failure, e.g. the user revoked access or deleted the file).
std::string mobileLastDocUri();
std::string mobileLastDocName();
std::string mobileOpenUri(const std::string& uri);
// True when the last mobileOpenUri() served an app-private BACKUP copy because
// the real document was gone or disowned. The returned file is real content,
// but it is NOT what that URI currently holds - so the caller must not keep the
// URI as a quick-save target, or a later save would truncate a document whose
// contents were never read. Callers unlink the project and tell the user.
bool mobileLastOpenWasFallback();

// Raise / dismiss the system soft keyboard. On Android this bypasses
// SDL_StartTextInput's SDL_GetFocusWindow()==NULL gate (null in the immersive
// surface, so SDL silently never raises the IME). On iOS SDL raises the
// keyboard itself, so these are no-ops there.
void mobileShowTextInput();
void mobileHideTextInput();

#else  // desktop: no-ops (the calls live behind #if MZ_MOBILE anyway)

inline bool mobileStartOpenDocument(const std::string&) { return false; }
inline bool mobileStartCreateDocument(const std::string&, const std::string&) { return false; }
inline bool mobilePollFileResult(std::string&) { return false; }
inline bool mobileCommitSave(const std::string&) { return false; }
inline bool mobileCommitSaveToRef(const std::string&, const std::string&) { return false; }
inline void mobileShareFile(const std::string&, const std::string&) {}
inline std::string mobileLastDocUri() { return {}; }
inline std::string mobileLastDocName() { return {}; }
inline std::string mobileOpenUri(const std::string&) { return {}; }
inline bool mobileLastOpenWasFallback() { return false; }
inline void mobileShowTextInput() {}
inline void mobileHideTextInput() {}

#endif

} // namespace materializr
