package com.materializr.app;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

// Minimal content provider so exported files in the app cache can be handed to
// the system share sheet as content:// URIs (file:// is blocked since Android 7).
// Self-contained - no AndroidX/support dependency. It only serves read-only files
// out of <cacheDir>/share/, by file name (the URI's last path segment).
public class MaterializrFileProvider extends ContentProvider {

    private File shareFile(Uri uri) {
        // Last path segment is the file name; confine strictly to the share dir.
        String name = new File(uri.getLastPathSegment()).getName();
        return new File(new File(getContext().getCacheDir(), "share"), name);
    }

    @Override
    public boolean onCreate() { return true; }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        return ParcelFileDescriptor.open(shareFile(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
                        String[] selectionArgs, String sortOrder) {
        File f = shareFile(uri);
        MatrixCursor c = new MatrixCursor(
            new String[]{ OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE }, 1);
        c.addRow(new Object[]{ f.getName(), f.length() });
        return c;
    }

    @Override
    public String getType(Uri uri) { return "application/octet-stream"; }

    @Override public Uri insert(Uri uri, ContentValues values) { return null; }
    @Override public int delete(Uri uri, String sel, String[] selArgs) { return 0; }
    @Override public int update(Uri uri, ContentValues v, String sel, String[] selArgs) { return 0; }
}
