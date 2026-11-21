package org.rsdkair.launcher;

import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.util.Log;

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
}
