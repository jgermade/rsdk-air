package org.rsdkair.launcher;

import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.util.Log;
import android.view.DisplayCutout;
import android.view.View;
import android.provider.OpenableColumns;
import android.view.WindowInsets;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Runs the picker (libmain.so with no arguments). When a game is chosen, native code
 * calls {@link #startGame}, which starts {@link GameActivity} in the ":game" process, or
 * {@link #startExternalGame} for a game played in another app (Sonic 3 A.I.R.).
 */
public class LauncherActivity extends SDLActivity {

    private static final String TAG = "rsdk-air";

    // Importing a data file (see importFile). The states match ImportState in
    // src/launcher/main.cpp.
    private static final int REQUEST_IMPORT = 0x5244;
    private static final int IMPORT_IDLE = 0;
    private static final int IMPORT_RUNNING = 1;
    private static final int IMPORT_DONE = 2;
    private static final int IMPORT_CANCELLED = 3;
    private static final int IMPORT_FAILED = 4;

    private volatile int importState = IMPORT_IDLE;
    private volatile long importDone = 0;
    private volatile long importTotal = -1;
    private volatile String importError = "";
    private volatile String importDest = null;

    /** Called from native code (src/launcher/main.cpp) on the SDL thread. */
    public void startGame(String gameId, String dataRoot) {
        Intent intent = new Intent(this, GameActivity.class);
        intent.putExtra(GameActivity.EXTRA_GAME_ID, gameId);
        intent.putExtra(GameActivity.EXTRA_DATA_ROOT, dataRoot);
        startActivity(intent);
    }

    /**
     * Called from native code. Whether another app is installed. The package must be
     * listed under queries in AndroidManifest.xml to be visible on Android 11+.
     */
    public boolean isAppInstalled(String packageName) {
        try {
            getPackageManager().getPackageInfo(packageName, 0);
            return true;
        } catch (PackageManager.NameNotFoundException e) {
            return false;
        }
    }

    /**
     * Called from native code. Opens a file in another app, the way a file manager would
     * (ACTION_VIEW, application/octet-stream), with read access to that one file only.
     * Returns false if the app cannot take it.
     */
    public boolean startExternalGame(String packageName, String filePath) {
        Uri uri = RomProvider.share(this, new File(filePath));
        Intent intent = new Intent(Intent.ACTION_VIEW);
        intent.setDataAndType(uri, "application/octet-stream");
        intent.setPackage(packageName);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try {
            startActivity(intent);
            return true;
        } catch (ActivityNotFoundException e) {
            Log.w(TAG, "cannot open " + filePath + " in " + packageName, e);
            return false;
        }
    }

    /**
     * Called from native code (src/launcher/picker.cpp). How many pixels of each screen
     * edge (left, top, right, bottom) are covered by visible system bars or by a display
     * cutout; the picker lays its content out inside the rest.
     */
    public int[] getSafeInsets() {
        int[] out = new int[4];
        View decor = getWindow() != null ? getWindow().getDecorView() : null;
        WindowInsets insets = decor != null ? decor.getRootWindowInsets() : null;
        if (insets == null) {
            return out;
        }
        if (Build.VERSION.SDK_INT >= 30 /* Android 11 */) {
            android.graphics.Insets i = insets.getInsets(
                    WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            out[0] = i.left;
            out[1] = i.top;
            out[2] = i.right;
            out[3] = i.bottom;
            return out;
        }
        out[0] = insets.getSystemWindowInsetLeft();
        out[1] = insets.getSystemWindowInsetTop();
        out[2] = insets.getSystemWindowInsetRight();
        out[3] = insets.getSystemWindowInsetBottom();
        if (Build.VERSION.SDK_INT >= 28 /* Android 9 */) {
            DisplayCutout cutout = insets.getDisplayCutout();
            if (cutout != null) {
                out[0] = Math.max(out[0], cutout.getSafeInsetLeft());
                out[1] = Math.max(out[1], cutout.getSafeInsetTop());
                out[2] = Math.max(out[2], cutout.getSafeInsetRight());
                out[3] = Math.max(out[3], cutout.getSafeInsetBottom());
            }
        }
        return out;
    }

    /**
     * Called from native code. Opens the system file picker; the file the player chooses
     * is copied to destPath (a game's data folder, private to the app) on a background
     * thread. Native code polls {@link #getImportStatus} until the state is no longer
     * running. Returns false if an import is already running.
     */
    public boolean importFile(String destPath) {
        if (importState == IMPORT_RUNNING) {
            return false;
        }
        importDest = destPath;
        importDone = 0;
        importTotal = -1;
        importError = "";
        importState = IMPORT_RUNNING;
        runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            try {
                startActivityForResult(intent, REQUEST_IMPORT);
            } catch (ActivityNotFoundException e) {
                importError = "this device has no file picker";
                importState = IMPORT_FAILED;
            }
        });
        return true;
    }

    /** Called from native code: {state, bytes copied, total bytes or -1}. */
    public long[] getImportStatus() {
        return new long[] {importState, importDone, importTotal};
    }

    /** Called from native code: why the last import failed. */
    public String getImportError() {
        return importError;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != REQUEST_IMPORT) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        final Uri uri = data != null ? data.getData() : null;
        if (resultCode != RESULT_OK || uri == null) {
            importState = IMPORT_CANCELLED;
            return;
        }
        final String dest = importDest;
        new Thread(() -> copyImport(uri, dest), "rsdk-air-import").start();
    }

    private void copyImport(Uri uri, String destPath) {
        File dest = new File(destPath);
        File part = new File(destPath + ".part");
        try {
            importTotal = querySize(uri);
            File dir = dest.getParentFile();
            if (dir != null && !dir.isDirectory() && !dir.mkdirs()) {
                throw new IOException("cannot create " + dir);
            }
            try (InputStream in = getContentResolver().openInputStream(uri);
                 OutputStream out = new FileOutputStream(part)) {
                if (in == null) {
                    throw new IOException("cannot open the chosen file");
                }
                byte[] buffer = new byte[1 << 16];
                long done = 0;
                int n;
                while ((n = in.read(buffer)) > 0) {
                    out.write(buffer, 0, n);
                    done += n;
                    importDone = done;
                }
            }
            // Replace the old file only once the copy is complete.
            if (dest.exists() && !dest.delete()) {
                throw new IOException("cannot replace " + dest);
            }
            if (!part.renameTo(dest)) {
                throw new IOException("cannot rename " + part + " to " + dest);
            }
            importState = IMPORT_DONE;
        } catch (IOException | SecurityException e) {
            Log.w(TAG, "import of " + uri + " failed", e);
            part.delete();
            importError = e.getMessage() != null ? e.getMessage() : e.toString();
            importState = IMPORT_FAILED;
        }
    }

    private long querySize(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, new String[] {OpenableColumns.SIZE}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) {
                return cursor.getLong(0);
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot read the size of " + uri, e);
        }
        return -1;
    }
}
