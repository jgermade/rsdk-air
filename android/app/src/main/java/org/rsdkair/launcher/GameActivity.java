package org.rsdkair.launcher;

import org.libsdl.app.SDLActivity;

/**
 * Runs one game in the ":game" process: libmain.so with "--game <id>", which loads that
 * game's engine module (lib&lt;engine&gt;.so) and hands it control. The process is killed
 * when the activity is destroyed, so every game starts from a clean process.
 */
public class GameActivity extends SDLActivity {

    static final String EXTRA_GAME_ID = "org.rsdkair.launcher.GAME_ID";
    static final String EXTRA_DATA_ROOT = "org.rsdkair.launcher.DATA_ROOT";

    @Override
    protected String[] getArguments() {
        String gameId = getIntent().getStringExtra(EXTRA_GAME_ID);
        String dataRoot = getIntent().getStringExtra(EXTRA_DATA_ROOT);
        if (gameId == null) {
            return new String[0];
        }
        if (dataRoot == null) {
            return new String[] {"--game", gameId};
        }
        return new String[] {"--game", gameId, "--data-root", dataRoot};
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        // Engines keep global state: never reuse this process for another game.
        android.os.Process.killProcess(android.os.Process.myPid());
    }
}
