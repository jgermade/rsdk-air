// See rsdkv4_bridge.hpp. Mirrors Debug.cpp move for move: the same globals, the same
// saveRAM layout, the same InitStartingStage() calls, so a game started here lands in
// exactly the state the engine's own menu would produce. Two differences, both marked
// below: no-save mode leaves saveRAM alone, and a new game skips the title screen.
#include "rsdkv4_bridge.hpp"

#include "RetroEngine.hpp"

namespace rsdkv4_bridge {

namespace {

// One save slot is 8 ints of saveRAM (Debug.cpp, STARTMENU_SAVESEL).
enum SaveField {
    SAVE_CHARACTER = 0, // 0 Sonic, 1 Tails, 2 Knuckles, 3 Sonic & Tails
    SAVE_LIVES,
    SAVE_SCORE,
    SAVE_BONUS,
    SAVE_ZONE, // 1-based next zone; 0 = unused slot; >127 = special stage
    SAVE_EMERALDS,
    SAVE_SPECIALPOS,
};

void clear_slot(int base, int character, int zone)
{
    saveRAM[base + SAVE_CHARACTER]  = character;
    saveRAM[base + SAVE_LIVES]      = 3;
    saveRAM[base + SAVE_SCORE]      = 0;
    saveRAM[base + SAVE_BONUS]      = 50000;
    saveRAM[base + SAVE_ZONE]       = zone;
    saveRAM[base + SAVE_EMERALDS]   = 0;
    saveRAM[base + SAVE_SPECIALPOS] = 0;
    saveRAM[base + 7]               = 0;
}

} // namespace

void enter_game_loop()
{
    ClearNativeObjects();
    CREATE_ENTITY(RetroGameLoop);
    if (Engine.gameDeviceType == RETRO_MOBILE)
        CREATE_ENTITY(VirtualDPad);
    Engine.gameMode = ENGINE_MAINGAME;
}

void delete_save(int slot)
{
    if (slot < 0 || slot > 3)
        return;
    clear_slot(slot << 3, 0, 0);
    WriteSaveRAMData();
}

void start_game(int slot, int player)
{
    if (player < 0 || player > 3)
        player = 0;

    if (slot >= 0 && slot <= 3 && saveRAM[(slot << 3) + SAVE_ZONE]) {
        const int base = slot << 3;
        SetGlobalVariableByName("options.saveSlot", slot);
        SetGlobalVariableByName("options.gameMode", 1);
        SetGlobalVariableByName("options.stageSelectFlag", 0);
        SetGlobalVariableByName("player.lives", saveRAM[base + SAVE_LIVES]);
        SetGlobalVariableByName("player.score", saveRAM[base + SAVE_SCORE]);
        SetGlobalVariableByName("player.scoreBonus", saveRAM[base + SAVE_BONUS]);
        SetGlobalVariableByName("specialStage.emeralds", saveRAM[base + SAVE_EMERALDS]);
        SetGlobalVariableByName("specialStage.listPos", saveRAM[base + SAVE_SPECIALPOS]);
        SetGlobalVariableByName("stage.player2Enabled", saveRAM[base + SAVE_CHARACTER] == 3);
        SetGlobalVariableByName("lampPostID", 0); // Sonic 1
        SetGlobalVariableByName("starPostID", 0); // Sonic 2
        SetGlobalVariableByName("options.vsMode", 0);

        const int nextZone = saveRAM[base + SAVE_ZONE];
        if (nextZone > 127) {
            SetGlobalVariableByName("specialStage.nextZone", nextZone - 129);
            InitStartingStage(STAGELIST_SPECIAL, saveRAM[base + SAVE_SPECIALPOS], saveRAM[base + SAVE_CHARACTER]);
        }
        else {
            SetGlobalVariableByName("specialStage.nextZone", nextZone - 1);
            InitStartingStage(STAGELIST_REGULAR, nextZone - 1, saveRAM[base + SAVE_CHARACTER]);
        }
        enter_game_loop();
        return;
    }

    // Unlike Debug.cpp, no-save mode does not write saveRAM: upstream stamps slot 1 with a
    // fresh game there, which is surprising when the player asked not to save.
    const bool saving = slot >= 0 && slot <= 3;
    if (saving) {
        clear_slot(slot << 3, player, 1);
        SetGlobalVariableByName("options.gameMode", 1);
        SetGlobalVariableByName("options.stageSelectFlag", 0);
        SetGlobalVariableByName("options.saveSlot", slot);
    }
    else {
        SetGlobalVariableByName("options.gameMode", 0);
        SetGlobalVariableByName("options.saveSlot", 0);
    }

    SetGlobalVariableByName("player.lives", 3);
    SetGlobalVariableByName("player.score", 0);
    SetGlobalVariableByName("player.scoreBonus", 50000);
    SetGlobalVariableByName("specialStage.emeralds", 0);
    SetGlobalVariableByName("specialStage.listPos", 0);
    SetGlobalVariableByName("stage.player2Enabled", player == 3);
    SetGlobalVariableByName("lampPostID", 0); // Sonic 1
    SetGlobalVariableByName("starPostID", 0); // Sonic 2
    SetGlobalVariableByName("options.vsMode", 0);

    if (saving)
        WriteSaveRAMData();

    // Straight into zone 1, as the engine's own StartingSaveFile path does
    // (RetroEngine.cpp, startSave). The start menu goes through Presentation/0 (the title
    // screen) instead, which a player who already picked a slot does not want to see again.
    SetGlobalVariableByName("specialStage.nextZone", 0);
    InitStartingStage(STAGELIST_REGULAR, 0, player);
    enter_game_loop();
}

} // namespace rsdkv4_bridge
