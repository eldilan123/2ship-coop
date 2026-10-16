// Test-only actions driven by --coop-test-* / --coop-autoload-slot, so the coop flows can be exercised without a
// keyboard: probar.ps1 starts the games with these options and checks the "[Coop]" log lines.
#include "CoopTest.h"

#include <chrono>
#include <spdlog/spdlog.h>
#include "Coop.h"
#include "CoopArgs.h"
#include "CoopClock.h"
#include "CoopSession.h"
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

#include "2s2h/SaveManager/SaveManager.h"
void SaveManager_DeleteSaveFile(const std::filesystem::path& fileName);
void SaveManager_MoveInvalidSaveFile(const std::filesystem::path& fileName, const std::string& message);

namespace Coop::Test {

namespace {

using SteadyClock = std::chrono::steady_clock;

bool sBootHandled = false;
bool sAutoloadDone = false;
int sFileSelectFrames = 0;
// First time this game reached gameplay (for a client: entering the session)
std::optional<SteadyClock::time_point> sGameplayStart;
size_t sNextGoto = 0;
size_t sNextTimeJump = 0;
bool sResetCycleDone = false;
bool sForceSaveDone = false;
int sBlockSongsStep = 0;
size_t sNextClientExit = 0;
size_t sNextForm = 0;
size_t sNextPos = 0;

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

// --coop-test-reset-cycle: does what the Song of Time does once the player answers "Yes" (Sram_SaveEndOfCycle, then
// the transition Player starts for OCARINA_MODE_APPLY_SOT), except that the host's save is not written to disk.
void UpdateResetCycle() {
    const std::optional<int>& at = GetLaunchOptions().testResetCycleAt;
    if (!at.has_value() || sResetCycleDone || GameplaySeconds() < *at || !NoTransitionInProgress()) {
        return;
    }
    sResetCycleDone = true;
    SPDLOG_INFO("[Coop] TEST: host resets the cycle like the Song of Time (its save file is not written)");
    SetTestSaveSuppression("test reset-cycle: the host's save file is not written");
    Sram_SaveEndOfCycle(gPlayState);
    SetTestSaveSuppression(nullptr);
    gPlayState->nextEntrance =
        gSaveContext.save.saveInfo.playerData.threeDayResetCount == 1 ? ENTRANCE(CUTSCENE, 1) : ENTRANCE(CUTSCENE, 0);
    gSaveContext.nextCutsceneIndex = 0xFFF7;
    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
}

// --coop-test-force-save: runs every save path on a client in a session. Each one must end in "[Coop] Save blocked"
// and probar.ps1 checks that the client's save files keep the same hash.
void ForceEverySavePath() {
    SPDLOG_INFO("[Coop] TEST: forcing every save path on the client");
    SramContext* sramCtx = &gPlayState->sramCtx;
    const s32 fileNum = gSaveContext.fileNum;
    const u8 isFirstCycle = gSaveContext.save.isFirstCycle;

    SPDLOG_INFO("[Coop] TEST: path 1/9: Song of Time, game over and moon crash save (default pages)");
    func_8014546C(sramCtx);
    Sram_SetFlashPagesDefault(sramCtx, gFlashSaveStartPages[fileNum * FLASH_SAVE_MAIN_MULTIPLIER],
                              gFlashSpecialSaveNumPages[fileNum * FLASH_SAVE_MAIN_MULTIPLIER]);
    Sram_StartWriteToFlashDefault(sramCtx);

    SPDLOG_INFO("[Coop] TEST: path 2/9: owl statue, autosave and pause save (owl pages)");
    Sram_SetFlashPagesOwlSave(sramCtx, gFlashOwlSaveStartPages[fileNum * FLASH_SAVE_MAIN_MULTIPLIER],
                              gFlashOwlSaveNumPages[fileNum * FLASH_SAVE_MAIN_MULTIPLIER]);
    Sram_StartWriteToFlashOwlSave(sramCtx);

    SPDLOG_INFO("[Coop] TEST: path 3/9: owl save deletion (file load, end of cycle, moon crash)");
    func_80147314(sramCtx, fileNum);

    SPDLOG_INFO("[Coop] TEST: path 4/9: special save when entering Clock Town (intro and Tatl skips)");
    Sram_SaveSpecialEnterClockTown(gPlayState);
    gSaveContext.save.isFirstCycle = isFirstCycle; // That routine also marks the first cycle in memory

    SPDLOG_INFO("[Coop] TEST: path 5/9: dropped JSON save import");
    char fakeJson[] = "coop-test-dropped-save.json";
    SaveManager_HandleFileDropped(fakeJson);

    SPDLOG_INFO("[Coop] TEST: path 6/9: dropped binary save import");
    char fakeBinary[] = "coop-test-dropped-save.sav";
    BinarySaveConverter_HandleFileDropped(fakeBinary);

    SPDLOG_INFO("[Coop] TEST: path 7/9: randomizer Saria hint persistence and direct save file write");
    SaveManager_PersistSariaHintsAvailable();
    SaveManager_WriteSaveFile("file1.json", nlohmann::json::object());

    SPDLOG_INFO("[Coop] TEST: path 8/9: delete file (erase, delete on death)");
    SaveManager_DeleteSaveFile("file1.json");

    SPDLOG_INFO("[Coop] TEST: path 9/9: rename of an invalid save file");
    SaveManager_MoveInvalidSaveFile("file1.json", "coop test");
    SPDLOG_INFO("[Coop] TEST: forced every save path");
}

void UpdateForceSave() {
    const std::optional<int>& at = GetLaunchOptions().testForceSaveAt;
    if (at.has_value() && !sForceSaveDone && IsClientInSession() && GameplaySeconds() >= *at &&
        NoTransitionInProgress()) {
        sForceSaveDone = true;
        ForceEverySavePath();
    }
}

// --coop-test-client-exit: sets up a scene exit at the end of the frame, as Player's loading-zone code does, so the
// session code sees it at the start of the next frame.
void UpdateClientExit() {
    const auto& exits = GetLaunchOptions().testClientExit;
    if (sNextClientExit >= exits.size() || !IsClientInSession() || !InGameplay() ||
        GameplaySeconds() < exits[sNextClientExit].atSeconds || !NoTransitionInProgress()) {
        return;
    }
    const uint16_t entrance = static_cast<uint16_t>(exits[sNextClientExit].value);
    sNextClientExit++;
    SPDLOG_INFO("[Coop] TEST: client walks into an exit to entrance 0x{:04X}", entrance);
    gPlayState->nextEntrance = entrance;
    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
}

// --coop-test-block-songs: leaves msgMode exactly as Message_Update does when the player answers "Yes" to the Song of
// Time and then to an owl statue. It runs at the end of the frame, so the coop frame hook sees it first next frame.
void OnGameStateMainFinish() {
    UpdateClientExit();
    const std::optional<int>& at = GetLaunchOptions().testBlockSongsAt;
    if (!at.has_value() || sBlockSongsStep >= 2 || !IsClientInSession() || !InGameplay() ||
        GameplaySeconds() < *at + sBlockSongsStep * 2) {
        return;
    }
    if (sBlockSongsStep == 0) {
        SPDLOG_INFO("[Coop] TEST: client answers \"Yes\" to the Song of Time");
        gPlayState->msgCtx.msgMode = MSGMODE_NEW_CYCLE_0;
    } else {
        SPDLOG_INFO("[Coop] TEST: client answers \"Yes\" to an owl statue save");
        gPlayState->msgCtx.msgMode = MSGMODE_OWL_SAVE_0;
    }
    sBlockSongsStep++;
}

// --coop-test-form: the same in-place transformation FastTransformation.cpp does (a copy of func_8012301C): the local
// form is written to the (per-player) save field and the Player actor re-inits with the new form's object.
void ChangeLocalForm(u8 form) {
    PlayState* play = gPlayState;
    Player* player = GET_PLAYER(play);
    SPDLOG_INFO("[Coop] TEST: local form {} -> {}", player->transformation, form);
    gSaveContext.save.playerForm = form;
    const s16 objectId = gPlayerFormObjectIds[form];
    gActorOverlayTable[ACTOR_PLAYER].profile->objectId = objectId;
    func_8012F73C(&play->objectCtx, player->actor.objectSlot, objectId);
    player->actor.objectSlot = Object_GetSlot(&play->objectCtx, objectId);
    player->actor.shape.rot.z = form + 1;
    player->actor.init = PlayerCall_Init;
    player->actor.update = PlayerCall_Update;
    player->actor.draw = PlayerCall_Draw;
    gSaveContext.save.equippedMask = PLAYER_MASK_NONE;
    if (player->transformation == PLAYER_FORM_HUMAN) {
        player->prevMask = PLAYER_MASK_NONE;
    }
}

// Local Link changes for --coop-test-form and --coop-test-pos (host and client).
void UpdateLocalChanges() {
    const LaunchOptions& options = GetLaunchOptions();
    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr || player->actor.update != PlayerCall_Update || !NoTransitionInProgress()) {
        return;
    }
    const int seconds = GameplaySeconds();
    if (sNextForm < options.testForm.size() && seconds >= options.testForm[sNextForm].atSeconds) {
        ChangeLocalForm(static_cast<u8>(options.testForm[sNextForm++].value));
        return;
    }
    if (sNextPos < options.testPos.size() && seconds >= options.testPos[sNextPos].atSeconds) {
        const LaunchOptions::TimedPos& pos = options.testPos[sNextPos++];
        SPDLOG_INFO("[Coop] TEST: local position -> ({:.1f}, {:.1f}, {:.1f})", pos.x, pos.y, pos.z);
        player->actor.world.pos = { pos.x, pos.y, pos.z };
    }
}

void RegisterTestHooks() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnConsoleLogoUpdate>(OnConsoleLogoUpdate);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameStateMainFinish>(OnGameStateMainFinish);
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
        UpdateResetCycle();
    } else {
        UpdateForceSave();
    }
    UpdateLocalChanges();
}

static RegisterShipInitFunc initFunc(RegisterTestHooks);

} // namespace Coop::Test
