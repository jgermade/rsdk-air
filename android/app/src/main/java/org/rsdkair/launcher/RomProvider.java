package org.rsdkair.launcher;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;
import java.util.HashMap;
import java.util.Map;
import java.util.UUID;

/**
 * Hands one file (the Sonic 3 A.I.R. ROM) to another app, read-only, through a
 * content:// URI. Not exported: the receiving app can only read a URI it was granted
 * with FLAG_GRANT_READ_URI_PERMISSION. Each shared file gets a random token, so the URI
 * never reveals or accepts a path.
 */
public class RomProvider extends ContentProvider {

    private static final Map<String, File> sShared = new HashMap<>();

    static String authority(Context context) {
        return context.getPackageName() + ".rom";
    }

    /** Registers the file and returns the URI to put in the Intent. */
    static synchronized Uri share(Context context, File file) {
        String token = UUID.randomUUID().toString();
        sShared.put(token, file);
        return new Uri.Builder().scheme("content").authority(authority(context))
                .appendPath(token).appendPath(file.getName()).build();
    }

    private static synchronized File lookup(Uri uri) {
        if (uri.getPathSegments().isEmpty()) return null;
        return sShared.get(uri.getPathSegments().get(0));
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        File file = lookup(uri);
        if (file == null || !"r".equals(mode)) throw new FileNotFoundException(uri.toString());
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public String getType(Uri uri) {
        return "application/octet-stream";
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        File file = lookup(uri);
        if (file == null) return null;
        MatrixCursor cursor = new MatrixCursor(new String[] { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE });
        cursor.addRow(new Object[] { file.getName(), file.length() });
        return cursor;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("read-only");
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("read-only");
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("read-only");
    }
}
