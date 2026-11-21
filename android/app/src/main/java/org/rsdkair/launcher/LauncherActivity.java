package org.rsdkair.launcher;

import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.util.Log;
import android.view.DisplayCutout;
import android.view.View;
import android.view.WindowInsets;

import org.libsdl.app.SDLActivity;

import java.io.File;

/**
 * Runs the picker (libmain.so with no arguments). When a game is chosen, native code
 * calls {@link #startGame}, which starts {@link GameActivity} in the ":game" process, or
 * {@link #startExternalGame} for a game played in another app (Sonic 3 A.I.R.).
 */
public class LauncherActivity extends SDLActivity {

    private static final String TAG = "rsdk-air";

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
}
