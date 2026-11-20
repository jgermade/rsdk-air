package org.rsdkair.launcher;

import android.content.Intent;

import org.libsdl.app.SDLActivity;

/**
 * Runs the picker (libmain.so with no arguments). When a game is chosen, native code
 * calls {@link #startGame}, which starts {@link GameActivity} in the ":game" process.
 */
public class LauncherActivity extends SDLActivity {

    /** Called from native code (src/launcher/main.cpp) on the SDL thread. */
    public void startGame(String gameId, String dataRoot) {
        Intent intent = new Intent(this, GameActivity.class);
        intent.putExtra(GameActivity.EXTRA_GAME_ID, gameId);
        intent.putExtra(GameActivity.EXTRA_DATA_ROOT, dataRoot);
        startActivity(intent);
    }
}
