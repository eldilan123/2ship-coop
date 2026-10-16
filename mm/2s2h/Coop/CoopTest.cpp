// Test-only actions driven by --coop-test-* / --coop-autoload-slot, so the coop flows can be exercised without a
// keyboard: probar.ps1 starts the games with these options and checks the "[Coop]" log lines.
#include "CoopTest.h"

#include <chrono>
#include <spdlog/spdlog.h>
#include "Coop.h"
#include "CoopArgs.h"
#include "CoopClock.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "overlays/gamestates/ovl_select/z_select.h"
#include "overlays/gamestates/ovl_file_choose/z_file_select.h"
extern FileSelectState* gFileSelectState;
}

namespace Coop::Test {

namespace {

using SteadyClock = std::chrono::steady_clock;

bool sBootHandled = false;
bool sAutoloadDone = false;
int sFileSelectFrames = 0;
std::optional<SteadyClock::time_point> sGameplayStart; // First time the host reached gameplay
size_t sNextGoto = 0;
size_t sNextTimeJump = 0;

bool InGameplay() {
    return gPlayState != nullptr && gSaveContext.gameMode == GAMEMODE_NORMAL;
}

int GameplaySeconds() {
    if (!sGameplayStart.has_value()) {
        return -1;
    }
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::seconds>(SteadyClock::now() - *sGameplayStart).count());
}

bool NoTransitionInProgress() {
    return gPlayState->transitionTrigger == TRANS_TRIGGER_OFF && gPlayState->transitionMode == TRANS_MODE_OFF;
}

// --coop-test-debug-save: like WarpPoint's boot-to-point, but on the game's debug save (fileNum 0xFF), which lives
// only in memory and is never written.
void BootIntoDebugSave(uint16_t entrance) {
    SPDLOG_INFO("[Coop] TEST: booting into gameplay on the debug save at entrance 0x{:04X}", entrance);
    gSaveContext.gameMode = GAMEMODE_NORMAL;
    gSaveContext.fileNum = 0xFF;
    MapSelect_LoadGame((MapSelectState*)gGameState, entrance, 0);
}

// --coop-autoload-slot: same as the "Skip to File Select" enhancement (normally run by the opening cutscene).
void BootToFileSelect() {
    SPDLOG_INFO("[Coop] TEST: going to file select to load slot {}", *GetLaunchOptions().autoloadSlot);
    Rand_Seed(osGetTime());
    gSaveContext.seqId = NA_BGM_DISABLED;
    gSaveContext.ambienceId = AMBIENCE_ID_DISABLED;
    Sram_InitNewSave();
    gSaveContext.save.time = CLOCK_TIME(8, 0);
    gSaveContext.save.day = 1;
    gSaveContext.save.playerForm = PLAYER_FORM_HUMAN;
    gSaveContext.gameMode = GAMEMODE_FILE_SELECT;
    STOP_GAMESTATE(gGameState);
    SET_NEXT_GAMESTATE(gGameState, FileSelect_Init, sizeof(FileSelectState));
}

void OnConsoleLogoUpdate() {
    if (sBootHandled) {
        return;
    }
    const LaunchOptions& options = GetLaunchOptions();
    if (options.testDebugSaveEntrance.has_value()) {
        sBootHandled = true;
        BootIntoDebugSave(*options.testDebugSaveEntrance);
    } else if (options.autoloadSlot.has_value()) {
        sBootHandled = true;
        BootToFileSelect();
    }
}

// Picks the slot on the file select main menu exactly as confirming it does: FileSelect_FadeOut, then
// FileSelect_LoadGame -> Sram_OpenSave -> Play.
void UpdateAutoload() {
    const std::optional<int>& slot = GetLaunchOptions().autoloadSlot;
    if (!slot.has_value() || sAutoloadDone || gFileSelectState == nullptr || gGameState != &gFileSelectState->state) {
        sFileSelectFrames = 0;
        return;
    }
    FileSelectState* fileSelect = gFileSelectState;
    if (fileSelect->menuMode != FS_MENU_MODE_CONFIG || fileSelect->configMode != CM_MAIN_MENU) {
        return;
    }
    // Let the main menu finish fading in.
    if (++sFileSelectFrames < 20) {
        return;
    }
    sAutoloadDone = true;
    const int index = *slot - 1;
    if (!SLOT_OCCUPIED(fileSelect, index)) {
        SPDLOG_WARN("[Coop] TEST: --coop-autoload-slot {}: that slot is empty", *slot);
        return;
    }
    SPDLOG_INFO("[Coop] TEST: loading slot {} from file select", *slot);
    fileSelect->buttonIndex = index;
    fileSelect->selectedFileIndex = index;
    fileSelect->menuMode = FS_MENU_MODE_SELECT;
    fileSelect->selectMode = SM_FADE_OUT;
}

// --coop-test-goto: host warps like the debug console "entrance" command.
void UpdateGoto() {
    const auto& gotos = GetLaunchOptions().testGoto;
    if (sNextGoto >= gotos.size() || GameplaySeconds() < gotos[sNextGoto].atSeconds || !NoTransitionInProgress()) {
        return;
    }
    const uint16_t entrance = static_cast<uint16_t>(gotos[sNextGoto].value);
    sNextGoto++;
    SPDLOG_INFO("[Coop] TEST: host going to entrance 0x{:04X}", entrance);
    gPlayState->nextEntrance = entrance;
    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
    gPlayState->transitionType = TRANS_TYPE_FADE_BLACK;
    gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK;
}

// --coop-test-timejump: moves the host clock; CoopClock notices the jump and sends it to the clients.
void UpdateTimeJump() {
    const auto& jumps = GetLaunchOptions().testTimeJump;
    if (sNextTimeJump >= jumps.size() || GameplaySeconds() < jumps[sNextTimeJump].atSeconds ||
        !NoTransitionInProgress()) {
        return;
    }
    const int minutes = jumps[sNextTimeJump].value;
    sNextTimeJump++;
    const uint16_t newTime = static_cast<uint16_t>(CURRENT_TIME + minutes * 0x10000 / 1440);
    SPDLOG_INFO("[Coop] TEST: host time jump of {} minutes", minutes);
    Clock::JumpTo(newTime, gSaveContext.save.day);
}

void RegisterTestHooks() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnConsoleLogoUpdate>(OnConsoleLogoUpdate);
}

} // namespace

void Frame() {
    UpdateAutoload();
    if (!InGameplay()) {
        return;
    }
    if (!sGameplayStart.has_value()) {
        sGameplayStart = SteadyClock::now();
    }
    if (IsHosting()) {
        UpdateGoto();
        UpdateTimeJump();
    }
}

static RegisterShipInitFunc initFunc(RegisterTestHooks);

} // namespace Coop::Test
