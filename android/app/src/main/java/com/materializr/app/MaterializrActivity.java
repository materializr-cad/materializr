package com.materializr.app;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.res.Configuration;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.view.View;
import android.view.inputmethod.InputMethodManager;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Comparator;

import org.libsdl.app.SDLActivity;

// Materializr's Android entry activity. SDLActivity does the heavy lifting:
// it creates the GL surface, loads the native libraries below, and calls
// SDL_main (defined in android_main.cpp) on its own thread.
public class MaterializrActivity extends SDLActivity {

    private static MaterializrActivity sInstance;

    // ---- Storage Access Framework + share bridge -----------------------------
    // The native file layer (FileDialogs.cpp) drives these via JNI and polls
    // pollFileResult() each frame for the async picker result. SAF gives a
    // content:// URI; since OCCT reads/writes plain paths, we copy through a
    // cache temp file: open = copy URI->temp then hand the temp path to native;
    // save = native writes a temp then we copy temp->URI.
    private static final int REQ_OPEN = 0xF11E;
    private static final int REQ_SAVE = 0xF12E;

    private static volatile boolean sResultReady = false;
    private static volatile String  sResultValue = "";   // open: temp path; save: "ok"; cancel: ""
    private Uri mPendingSaveUri;                          // destination chosen for a save
    private static volatile String sLastDocUri  = "";    // persisted URI of the last open/save
    private static volatile String sLastDocName = "";    // its display name (for Open Recent)

    // Native -> Java entry points (called from FileDialogs.cpp via JNI) ---------

    // Launch the system open picker. mimeCsv is comma-separated MIME types (or
    // "*/*"). The result arrives via onActivityResult -> pollFileResult().
    public static void nativeOpenDocument(String mimeCsv) {
        final MaterializrActivity a = sInstance;
        if (a == null) return;
        a.runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            applyMimes(i, mimeCsv);
            // Request a *persistable* read grant so the picked document can be
            // re-opened later from the Open Recent list.
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                     | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            try { a.startActivityForResult(i, REQ_OPEN); }
            catch (Exception e) { signal(""); }
        });
    }

    // Launch the system "save as" picker with a suggested name + MIME.
    public static void nativeCreateDocument(String name, String mime) {
        final MaterializrActivity a = sInstance;
        if (a == null) return;
        a.runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType((mime == null || mime.isEmpty()) ? "application/octet-stream" : mime);
            i.putExtra(Intent.EXTRA_TITLE, name);
            // Persistable read+write so a saved project lands in Open Recent too.
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                     | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                     | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            try { a.startActivityForResult(i, REQ_SAVE); }
            catch (Exception e) { signal(""); }
        });
    }

    // Overwrite an ALREADY-PICKED document in place (quick-save): we hold a
    // persistable write grant on the URI from the original picker, so no new
    // picker - and no "name (1)" dedup copies - is involved. "wt" truncates.
    public static boolean nativeCommitSaveToUri(String uriStr, String tempPath) {
        MaterializrActivity a = sInstance;
        if (a == null || uriStr == null || uriStr.isEmpty()) return false;
        try {
            try (InputStream in = new FileInputStream(tempPath);
                 OutputStream out = a.getContentResolver()
                                     .openOutputStream(Uri.parse(uriStr), "wt")) {
                copy(in, out);
            }
            // Only AFTER the stream closed cleanly. Cloud providers do the
            // actual upload at close(), so a fallback written inside the
            // try-with-resources would record content the document never
            // received - leaving the backup copy newer than the real file
            // after a save the user was told had failed.
            try {
                File dir = new File(a.getCacheDir(), "import");
                dir.mkdirs();
                File named = new File(dir, a.queryName(Uri.parse(uriStr)));
                try (InputStream in2 = new FileInputStream(tempPath);
                     OutputStream out2 = new FileOutputStream(named)) { copy(in2, out2); }
                a.writeDocFallback(uriStr, named);
            } catch (Exception ignored) {}
            return true;
        } catch (Exception e) {
            android.util.Log.w("Materializr",
                "nativeCommitSaveToUri failed for " + uriStr, e);
            return false;
        }
    }

    // After native has written tempPath, copy it into the save destination the
    // user picked. Returns true on success.
    public static boolean nativeCommitSave(String tempPath) {
        MaterializrActivity a = sInstance;
        if (a == null || a.mPendingSaveUri == null) return false;
        // "wt" (truncate), not "w": some providers keep the old tail when the
        // new content is shorter, corrupting the gzip container.
        try (InputStream in = new FileInputStream(tempPath);
             OutputStream out = a.getContentResolver().openOutputStream(a.mPendingSaveUri, "wt")) {
            copy(in, out);
            return true;
        } catch (Exception e) {
            return false;
        } finally {
            a.mPendingSaveUri = null;
            new File(tempPath).delete();
        }
    }

    // Share a just-written file via the system share sheet. Copies it into
    // <cache>/share (served by MaterializrFileProvider) and fires ACTION_SEND.
    public static void nativeShareFile(String path, String mime) {
        final MaterializrActivity a = sInstance;
        if (a == null) { android.util.Log.e("MZSHARE", "no activity"); return; }
        a.runOnUiThread(() -> {
            try {
                File src = new File(path);
                File dir = new File(a.getCacheDir(), "share");
                dir.mkdirs();
                File dst = new File(dir, src.getName());
                try (InputStream in = new FileInputStream(src);
                     OutputStream out = new FileOutputStream(dst)) { copy(in, out); }
                Uri uri = Uri.parse("content://" + a.getPackageName() + ".fileprovider/" + dst.getName());
                Intent send = new Intent(Intent.ACTION_SEND);
                send.setType((mime == null || mime.isEmpty()) ? "application/octet-stream" : mime);
                send.putExtra(Intent.EXTRA_STREAM, uri);
                // ClipData + flag make the read grant propagate to the chosen app.
                send.setClipData(android.content.ClipData.newRawUri(dst.getName(), uri));
                send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                a.startActivity(Intent.createChooser(send, "Share " + dst.getName())
                        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION));
            } catch (Exception e) {
                android.util.Log.e("MZSHARE", "share failed", e);
            }
        });
    }

    // Native polls this each frame while a picker is open. Returns null while
    // pending, "" on cancel, or (open) the temp path / (save) "ok" when ready.
    public static String pollFileResult() {
        if (!sResultReady) return null;
        sResultReady = false;
        return sResultValue;
    }

    // Raise the soft keyboard directly. SDL_StartTextInput only shows the IME
    // when SDL_GetFocusWindow() is non-null, which it isn't in our immersive
    // surface - so SDL never calls this. Driving showTextInput (inherited from
    // SDLActivity) ourselves sets up SDL's text-routing DummyEdit and the
    // SHOW_FORCED patch raises the keyboard. Typed text still flows to SDL/ImGui.
    public static void nativeShowKeyboard() {
        try { showTextInput(0, 0, 1, 1); } catch (Exception ignored) {}
    }

    // Dismiss the soft keyboard (best-effort; paired with SDL_StopTextInput).
    public static void nativeHideKeyboard() {
        final MaterializrActivity a = sInstance;
        if (a == null) return;
        a.runOnUiThread(() -> {
            try {
                InputMethodManager imm = (InputMethodManager)
                        a.getSystemService(Context.INPUT_METHOD_SERVICE);
                View v = a.getWindow().getDecorView();
                if (imm != null && v != null)
                    imm.hideSoftInputFromWindow(v.getWindowToken(), 0);
            } catch (Exception ignored) {}
        });
    }

    // Persisted-document URI accessors for the Open Recent list.
    public static String nativeLastDocUri()  { return sLastDocUri; }
    public static String nativeLastDocName() { return sLastDocName; }

    // Re-open a previously persisted document URI without a picker: copy it into
    // a cache temp and return that path ("" on failure - access revoked or the
    // file was deleted). Runs synchronously on the caller (native) thread.
    public static String nativeOpenUri(String uriString) {
        MaterializrActivity a = sInstance;
        if (a == null || uriString == null || uriString.isEmpty()) return "";
        try {
            Uri uri = Uri.parse(uriString);
            File dir = new File(a.getCacheDir(), "import");
            dir.mkdirs();
            File dst = new File(dir, a.queryName(uri));
            try (InputStream in = a.getContentResolver().openInputStream(uri);
                 OutputStream out = new FileOutputStream(dst)) { copy(in, out); }
            a.rememberDoc(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            a.writeDocFallback(uriString, dst);
            return dst.getAbsolutePath();
        } catch (Exception e) {
            // Persisted SAF grants are NOT durable in practice: they're wiped
            // by uninstall / clear-data, and the Downloads provider's numeric
            // document ids churn on re-index - either way openInputStream
            // throws and every recent silently died here (the long-standing
            // "access may have been revoked" bug, diagnosed 2026-07-28).
            // Serve the app-private fallback copy from the last successful
            // open/save instead.
            //
            // But NOT when the document demonstrably still exists: that read
            // failed for a transient reason (a cloud provider offline or a
            // document it hasn't cached, storage unmounted, provider process
            // restarting), and the write grant is very much alive. Opening a
            // stale copy there and letting quick-save commit it is how you
            // silently overwrite newer work with older work. Fail visibly
            // instead - the user retries when they're back online.
            android.util.Log.w("Materializr",
                "nativeOpenUri: resolver failed for " + uriString, e);
            if (a.documentStillExists(uriString)) {
                android.util.Log.w("Materializr",
                    "nativeOpenUri: document exists but is unreachable - "
                    + "refusing the stale fallback");
                return "";
            }
            String fb = a.readDocFallback(uriString);
            if (!fb.isEmpty()) {
                // The caller must not treat this as the real document: the
                // original is gone or unreachable, so quick-saving back to
                // that URI could overwrite a file we never actually read.
                // Native drops the save identity and says so (see
                // mobileLastOpenWasFallback).
                sLastOpenWasFallback = true;
                android.util.Log.w("Materializr",
                    "nativeOpenUri: using fallback copy " + fb);
            }
            return fb;
        }
    }

    // True when the most recent nativeOpenUri() served an app-private fallback
    // copy rather than the real document. Native reads this right after the
    // open and unlinks the project from its content:// URI, so the next save
    // goes through the picker instead of truncating a document whose current
    // contents we never saw.
    private static boolean sLastOpenWasFallback = false;
    public static boolean nativeLastOpenWasFallback() { return sLastOpenWasFallback; }

    // Does the provider still have this document? Distinguishes "unreachable
    // right now" (row present -> the read failure was transient) from "gone or
    // disowned" (no row, or the query itself is refused -> the grant/doc-id
    // died, which is exactly what the fallback copies exist for). Note a
    // deleted file and a churned Downloads document id look identical here -
    // both come back as no row - so both take the fallback path, and the
    // unlink above is what keeps the deleted-file case from writing back.
    private boolean documentStillExists(String uriString) {
        try {
            Uri uri = Uri.parse(uriString);
            try (Cursor c = getContentResolver().query(
                     uri, new String[]{ DocumentsContract.Document.COLUMN_DOCUMENT_ID },
                     null, null, null)) {
                return c != null && c.moveToFirst();
            }
        } catch (Exception e) {
            return false;   // refused or unqueryable - treat as gone
        }
    }

    // ---- App-private fallback copies for Open Recent -------------------------
    // files/docfallback/<sha1(uri)>_<displayName>. Written on every successful
    // open and save-commit; read when the content resolver can no longer serve
    // the URI (see nativeOpenUri). Pruned to the newest 15 - same order of
    // magnitude as the recents list itself.
    private File docFallbackDir() {
        File d = new File(getFilesDir(), "docfallback");
        d.mkdirs();
        return d;
    }

    private static String sha1Hex(String s) {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-1");
            byte[] h = md.digest(s.getBytes("UTF-8"));
            StringBuilder sb = new StringBuilder();
            for (byte b : h) sb.append(String.format("%02x", b));
            return sb.toString();
        } catch (Exception e) {
            return Integer.toHexString(s.hashCode());
        }
    }

    private void writeDocFallback(String uriString, File src) {
        try {
            String key = sha1Hex(uriString);
            File dir = docFallbackDir();
            // One fallback per document: drop any older copy under a
            // different display name before writing the current one.
            File[] old = dir.listFiles((d, n) -> n.startsWith(key + "_"));
            if (old != null) for (File f : old) f.delete();
            File dst = new File(dir, key + "_" + src.getName());
            try (InputStream in = new FileInputStream(src);
                 OutputStream out = new FileOutputStream(dst)) { copy(in, out); }
            pruneDocFallback(dir);
        } catch (Exception e) {
            android.util.Log.w("Materializr", "writeDocFallback failed", e);
        }
    }

    private String readDocFallback(String uriString) {
        try {
            String key = sha1Hex(uriString);
            File[] hits = docFallbackDir().listFiles((d, n) -> n.startsWith(key + "_"));
            if (hits == null || hits.length == 0) return "";
            File src = hits[0];
            // Hand back a cache temp named like the original document, so the
            // native side sees the same shape as a live-URI open.
            File dir = new File(getCacheDir(), "import");
            dir.mkdirs();
            File dst = new File(dir, src.getName().substring(key.length() + 1));
            try (InputStream in = new FileInputStream(src);
                 OutputStream out = new FileOutputStream(dst)) { copy(in, out); }
            return dst.getAbsolutePath();
        } catch (Exception e) {
            android.util.Log.w("Materializr", "readDocFallback failed", e);
            return "";
        }
    }

    private static void pruneDocFallback(File dir) {
        File[] all = dir.listFiles();
        if (all == null || all.length <= 15) return;
        Arrays.sort(all, Comparator.comparingLong(File::lastModified));
        for (int i = 0; i < all.length - 15; ++i) all[i].delete();
    }

    private static void signal(String value) { sResultValue = value; sResultReady = true; }

    private static void applyMimes(Intent i, String mimeCsv) {
        if (mimeCsv == null || mimeCsv.isEmpty() || mimeCsv.equals("*/*")) {
            i.setType("*/*");
            return;
        }
        String[] mimes = mimeCsv.split(",");
        i.setType(mimes.length == 1 ? mimes[0] : "*/*");
        if (mimes.length > 1) i.putExtra(Intent.EXTRA_MIME_TYPES, mimes);
    }

    private static void copy(InputStream in, OutputStream out) throws Exception {
        byte[] buf = new byte[1 << 16];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        out.flush();
    }

    private String queryName(Uri uri) {
        String name = "import.bin";
        try (android.database.Cursor c = getContentResolver().query(uri, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (idx >= 0) { String n = c.getString(idx); if (n != null && !n.isEmpty()) name = n; }
            }
        } catch (Exception ignored) {}
        return new File(name).getName();
    }

    // Take a persistable permission on `uri` and record it as the last document
    // for the Open Recent list. Best-effort: some providers don't grant
    // persistable permissions, so recents may not be able to re-open those.
    private void rememberDoc(Uri uri, int modeFlags) {
        try {
            getContentResolver().takePersistableUriPermission(uri, modeFlags);
        } catch (Exception ignored) {}
        sLastDocUri = uri.toString();
        sLastDocName = queryName(uri);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQ_OPEN && requestCode != REQ_SAVE) return;
        Uri uri = (resultCode == Activity.RESULT_OK && data != null) ? data.getData() : null;
        if (uri == null) { signal(""); return; }  // cancelled
        if (requestCode == REQ_OPEN) {
            // Copy the chosen document into a cache temp file and hand back its path.
            try {
                File dir = new File(getCacheDir(), "import");
                dir.mkdirs();
                File dst = new File(dir, queryName(uri));
                try (InputStream in = getContentResolver().openInputStream(uri);
                     OutputStream out = new FileOutputStream(dst)) { copy(in, out); }
                rememberDoc(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                writeDocFallback(uri.toString(), dst);
                signal(dst.getAbsolutePath());
            } catch (Exception e) {
                android.util.Log.w("Materializr", "open-result copy failed", e);
                signal("");
            }
        } else { // REQ_SAVE: remember the destination; native writes a temp then commits.
            mPendingSaveUri = uri;
            rememberDoc(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION
                           | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            signal("ok");
        }
    }

    // ---- Window mode (immersive on a bare tablet, windowed in a desktop dock) -

    private boolean isDesktopMode() {
        Configuration c = getResources().getConfiguration();
        boolean hwKeyboard = c.keyboard == Configuration.KEYBOARD_QWERTY
                && c.hardKeyboardHidden == Configuration.HARDKEYBOARDHIDDEN_NO;
        boolean multiWindow = Build.VERSION.SDK_INT >= Build.VERSION_CODES.N
                && isInMultiWindowMode();
        return hwKeyboard || multiWindow;
    }

    private void applyWindowMode() {
        View dv = getWindow().getDecorView();
        if (!isDesktopMode()) {
            dv.setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        } else {
            dv.setSystemUiVisibility(View.SYSTEM_UI_FLAG_VISIBLE);
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        sInstance = this;
        // No storage-permission prompt: file open/save use the system SAF picker
        // (per-URI access), and export sharing uses the FileProvider.
    }

    @Override
    protected void onDestroy() {
        if (sInstance == this) sInstance = null;
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) applyWindowMode();
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        applyWindowMode();
    }

    @Override
    public void onMultiWindowModeChanged(boolean isInMultiWindowMode, Configuration newConfig) {
        super.onMultiWindowModeChanged(isInMultiWindowMode, newConfig);
        applyWindowMode();
    }

    @Override
    protected String[] getLibraries() {
        // libmain.so links the OCCT toolkits via DT_NEEDED, so the dynamic
        // loader pulls them (and libc++_shared) from the APK automatically.
        return new String[] {
            "SDL2",
            "main"
        };
    }
}
