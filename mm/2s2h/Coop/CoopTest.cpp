// Test-only actions driven by --coop-test-* / --coop-autoload-slot, so the coop flows can be exercised without a
// keyboard: probar.ps1 starts the games with these options and checks the "[Coop]" log lines.
#include "CoopTest.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include "Coop.h"
#include "CoopArgs.h"
#include "CoopClock.h"
#include "CoopCombat.h"
#include "CoopEnemies.h"
#include "CoopEnemyTypes.h"
#include "CoopInventory.h"
#include "CoopPlayerHits.h"
#include "CoopProjectiles.h"
#include "CoopRooms.h"
#include "CoopSession.h"
#include "CoopShared.h"
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
size_t sNextSpawn = 0;
int sLastSpawnSecond = -1;
int sSpawnIndex = 0;
size_t sNextKill = 0;
bool sFuzzDone = false;
int sSweepDone = -1;
// --coop-test-attack: presses in progress (game input, applied in OnPassPlayerInputs)
struct AttackState {
    int done = 0;
    SteadyClock::time_point next = {};
};
std::vector<AttackState> sAttacks;
std::vector<AttackState> sShots;
std::vector<AttackState> sHits;
size_t sNextNear = 0;
uint16_t sHeldButtons = 0;
bool sHeldStick = false;
s16 sStickYaw = 0; // World direction the stick pushes towards while held
bool sPressEdge = false;
SteadyClock::time_point sHeldUntil = {};
// --coop-test-cross: walking through a transition (stick held towards sStickYaw until then), presses of A left for a
// door, and the result line
SteadyClock::time_point sWalkUntil = {};
struct Cross {
    bool active = false;
    int index = -1;
    int8_t fromRoom = -1;
    int pressesLeft = 0;
    SteadyClock::time_point nextPress = {};
    SteadyClock::time_point reportAt = {};
    Vec3f placed = {};
    s16 facing = 0;
    int replaced = 0;
} sCross;
size_t sNextCross = 0;
int sCrossRetries = 0;
SteadyClock::time_point sRetryAt = {};
constexpr int kSweepStepSeconds = 5;

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
        // home.pos too: Player checks its move against walls from there (it is its previous position).
        player->actor.world.pos = player->actor.home.pos = { pos.x, pos.y, pos.z };
    }
}

// --coop-test-spawn / --coop-test-kill (host) and --coop-test-fuzz-enemies (both).
void UpdateEnemyTests() {
    const LaunchOptions& options = GetLaunchOptions();
    const int seconds = GameplaySeconds();
    if (IsHosting() && NoTransitionInProgress()) {
        while (sNextSpawn < options.testSpawn.size() && seconds >= options.testSpawn[sNextSpawn].atSeconds) {
            const LaunchOptions::TimedSpawn& spawn = options.testSpawn[sNextSpawn++];
            // Several spawns at the same second go around Link instead of on top of each other.
            sSpawnIndex = spawn.atSeconds == sLastSpawnSecond ? sSpawnIndex + 1 : 0;
            sLastSpawnSecond = spawn.atSeconds;
            Enemies::TestSpawn(static_cast<int16_t>(spawn.actorId), static_cast<uint16_t>(spawn.params), sSpawnIndex);
        }
        while (sNextKill < options.testKill.size() && seconds >= options.testKill[sNextKill].atSeconds) {
            Enemies::TestKill(options.testKill[sNextKill++].value);
        }
    }
    if (IsHosting() && !options.testSweep.empty() && NoTransitionInProgress() && seconds >= options.testSweepAt) {
        const int total = static_cast<int>(options.testSweep.size());
        const int step = (seconds - options.testSweepAt) / kSweepStepSeconds;
        if (step > sSweepDone && sSweepDone < total) {
            sSweepDone = std::min(step, total);
            if (sSweepDone < total) {
                const LaunchOptions::SweepEntry& entry = options.testSweep[sSweepDone];
                Enemies::TestSweepStep(sSweepDone, total, static_cast<int16_t>(entry.actorId),
                                       static_cast<uint16_t>(entry.params));
            } else {
                Enemies::TestSweepStep(total, total, -1, 0); // Only removes the last one
            }
        }
    }
    const std::optional<int>& limitFrom = options.testBombLimitFrom;
    static int sLastLimitSecond = -1;
    if (limitFrom.has_value() && seconds >= *limitFrom && seconds < *limitFrom + 60 && seconds != sLastLimitSecond) {
        sLastLimitSecond = seconds;
        Projectiles::TestBombLimit();
    }
    const std::optional<int>& fuzzAt = options.testFuzzEnemiesAt;
    if (fuzzAt.has_value() && !sFuzzDone && seconds >= *fuzzAt && (IsHosting() || IsClientPlayingInSession())) {
        sFuzzDone = true;
        Enemies::TestFuzz();
        Combat::TestFuzz();
        Projectiles::TestFuzz();
        PlayerHits::TestFuzz();
        Rooms::TestFuzz();
        Shared::TestFuzz();
        Inventory::TestFuzz();
    }
}

// --coop-test-attack: puts the local Link `distance` in front of the nearest enemy, facing it, on the floor.
void ApproachNearestEnemy(int distance) {
    PlayState* play = gPlayState;
    Player* player = GET_PLAYER(play);
    Actor* enemy = Enemies::NearestEnemy(player->actor.world.pos);
    if (enemy == nullptr) {
        SPDLOG_INFO("[Coop] TEST: no enemy to approach");
        return;
    }
    s16 yawFromEnemy = Math_Vec3f_Yaw(&enemy->world.pos, &player->actor.world.pos);
    Vec3f pos;
    // The spot needs a floor that is not a scene exit and no wall between the enemy and it (with room for the Link):
    // a flying enemy drifts to the edge of a field, a town gate or a building's door is a scene exit, and stepping on
    // one would take the whole session to another scene. Otherwise the enemy's other side, otherwise nowhere.
    bool found = false;
    for (int side = 0; side < 2 && !found; side++) {
        if (side == 1) {
            yawFromEnemy = static_cast<s16>(yawFromEnemy + 0x8000);
        }
        const f32 dx = Math_SinS(yawFromEnemy), dz = Math_CosS(yawFromEnemy);
        pos = { enemy->world.pos.x + distance * dx, enemy->world.pos.y + 50.0f, enemy->world.pos.z + distance * dz };
        CollisionPoly* poly = nullptr;
        s32 bgId = 0;
        const f32 floor = BgCheck_EntityRaycastFloor5(&play->colCtx, &poly, &bgId, &player->actor, &pos);
        if (poly == nullptr || SurfaceType_GetSceneExitIndex(&play->colCtx, poly, bgId) != 0) {
            continue;
        }
        pos.y = floor;
        const f32 out = distance < 0 ? -30.0f : 30.0f;
        Vec3f from = { enemy->world.pos.x, floor + 30.0f, enemy->world.pos.z };
        Vec3f to = { pos.x + out * dx, floor + 30.0f, pos.z + out * dz };
        Vec3f hit;
        CollisionPoly* wall = nullptr;
        s32 wallBgId = 0;
        found = !BgCheck_EntityLineTest1(&play->colCtx, &from, &to, &hit, &wall, true, false, false, false, &wallBgId);
    }
    if (!found) {
        SPDLOG_INFO("[Coop] TEST: no spot next to {} (no floor, a scene exit or a wall on both sides); the Link stays",
                    Enemies::TypeName(enemy->id));
        return;
    }
    // With a negative distance the Link ends up on the enemy's other side, still facing it.
    const s16 facing = static_cast<s16>(distance < 0 ? yawFromEnemy : yawFromEnemy + 0x8000);
    // home.pos too: Player checks its move against walls from there (its previous position), and a wall crossed on the
    // way (a scene exit among them) would stop it or take the exit.
    player->actor.world.pos = pos;
    player->actor.prevPos = pos;
    player->actor.home.pos = pos;
    player->actor.shape.rot.y = player->actor.world.rot.y = player->yaw = facing;
    SPDLOG_INFO("[Coop] TEST: Link placed {} units from {} at ({:.0f}, {:.0f}, {:.0f})", distance,
                Enemies::TypeName(enemy->id), pos.x, pos.y, pos.z);
}

void UpdateAttacks() {
    const auto& attacks = GetLaunchOptions().testAttack;
    sAttacks.resize(attacks.size());
    const auto now = SteadyClock::now();
    if (sHeldButtons != 0 || now < sHeldUntil) {
        return; // One press at a time
    }
    const int seconds = GameplaySeconds();
    for (size_t i = 0; i < attacks.size(); i++) {
        const LaunchOptions::TimedAttack& a = attacks[i];
        AttackState& state = sAttacks[i];
        if (seconds < a.atSeconds || state.done >= a.count || now < state.next) {
            continue;
        }
        if (a.approach > 0) {
            ApproachNearestEnemy(a.approach);
        }
        const std::string& b = a.button;
        sHeldButtons = b == "b" ? BTN_B : b == "z" ? BTN_Z : b == "r" ? BTN_R : BTN_A;
        sHeldStick = b == "roll";
        sStickYaw = GET_PLAYER(gPlayState)->actor.shape.rot.y;
        sPressEdge = true;
        sHeldUntil = now + std::chrono::milliseconds(a.holdMs);
        state.done++;
        state.next = now + std::chrono::milliseconds(a.intervalMs);
        SPDLOG_INFO("[Coop] TEST: press {} ({}/{}, {} ms)", b, state.done, a.count, a.holdMs);
        return;
    }
}

// --coop-test-shoot: a projectile from the local Link at the nearest enemy, `count` times.
void UpdateShots() {
    const auto& shots = GetLaunchOptions().testShoot;
    sShots.resize(shots.size());
    const auto now = SteadyClock::now();
    const int seconds = GameplaySeconds();
    for (size_t i = 0; i < shots.size(); i++) {
        const LaunchOptions::TimedAttack& s = shots[i];
        AttackState& state = sShots[i];
        if (seconds < s.atSeconds || state.done >= s.count || now < state.next) {
            continue;
        }
        if (s.approach > 0) {
            ApproachNearestEnemy(s.approach);
        }
        state.done++;
        state.next = now + std::chrono::milliseconds(s.intervalMs);
        Projectiles::TestShoot(s.button.c_str());
        return; // One per frame
    }
}

// Applied to the local Link's input only (Player passes its own input through this hook).
void OnPassPlayerInputs(Input* input) {
    if (sHeldButtons == 0 && SteadyClock::now() < sWalkUntil && gPlayState != nullptr) {
        const s16 angle = static_cast<s16>(sStickYaw - Camera_GetInputDirYaw(GET_ACTIVE_CAM(gPlayState)));
        input->cur.stick_x = input->rel.stick_x = static_cast<s8>(-Math_SinS(angle) * 60.0f);
        input->cur.stick_y = input->rel.stick_y = static_cast<s8>(Math_CosS(angle) * 60.0f);
        return;
    }
    if (sHeldButtons == 0) {
        return;
    }
    if (SteadyClock::now() >= sHeldUntil) {
        input->rel.button |= sHeldButtons;
        sHeldButtons = 0;
        sHeldStick = false;
        return;
    }
    input->cur.button |= sHeldButtons;
    if (sPressEdge) {
        input->press.button |= sHeldButtons;
        sPressEdge = false;
    }
    if (sHeldStick) {
        // Player turns the stick into a world direction relative to the camera: push towards the Link's facing
        // (the enemy, after the approach) whatever the camera does.
        const s16 angle = static_cast<s16>(sStickYaw - Camera_GetInputDirYaw(GET_ACTIVE_CAM(gPlayState)));
        const s8 x = static_cast<s8>(-Math_SinS(angle) * 80.0f);
        const s8 y = static_cast<s8>(Math_CosS(angle) * 80.0f);
        input->cur.stick_x = input->rel.stick_x = x;
        input->cur.stick_y = input->rel.stick_y = y;
    }
}

// --coop-test-hold: the local game held in a state (pause menu, text box, cutscene, knocked back), to test that
// room moves wait for it. Game input and game functions only.
struct Hold {
    bool active = false;
    std::string kind;
    SteadyClock::time_point until = {};
    SteadyClock::time_point nextKnock = {};
    bool closing = false;
};
Hold sHold;
size_t sNextHold = 0;

// --coop-test-change: this game's shared state changed through the game's own functions.
size_t sNextChange = 0;
// "open": A presses left for the chest (one, then more for the get-item text)
int sOpenPresses = 0;
SteadyClock::time_point sOpenNext = {};

// One A for the Link (opening the chest), then for up to 15 s an A every second while a text is open: the get-item
// text comes after the chest's animation and reads the controller itself (Message_ShouldAdvance), not the Link's
// input.
void UpdateOpenPresses() {
    const auto now = SteadyClock::now();
    if (sOpenPresses == 0 || now < sOpenNext || sHeldButtons != 0) {
        return;
    }
    sOpenNext = now + std::chrono::milliseconds(1000);
    sOpenPresses--;
    if (sOpenPresses == 14) {
        sHeldButtons = BTN_A;
        sPressEdge = true;
        sHeldUntil = now + std::chrono::milliseconds(100);
    } else if (gPlayState->msgCtx.msgMode != MSGMODE_NONE) {
        Input* input = CONTROLLER1(&gPlayState->state);
        input->cur.button |= BTN_A;
        input->press.button |= BTN_A;
    }
}

void UpdateChanges() {
    UpdateOpenPresses();
    const auto& changes = GetLaunchOptions().testChange;
    if (sNextChange >= changes.size() || !NoTransitionInProgress()) {
        return;
    }
    const int64_t wallMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    const LaunchOptions::TimedChange& next = changes[sNextChange];
    if (next.atUnixMs > 0 ? wallMs < next.atUnixMs : GameplaySeconds() < next.atSeconds) {
        return;
    }
    const LaunchOptions::TimedChange& c = changes[sNextChange++];
    if (c.kind == "item") {
        Item_Give(gPlayState, static_cast<u8>(c.value));
    } else if (c.kind == "rupees") {
        Rupees_ChangeBy(static_cast<s16>(c.value));
    } else if (c.kind == "week") {
        SET_WEEKEVENTREG(c.value);
    } else if (c.kind == "switch") {
        Flags_SetSwitch(gPlayState, c.value);
    } else if (c.kind == "use") {
        Inventory_ChangeAmmo(static_cast<s16>(c.value), -1);
    } else if (c.kind == "chest" || c.kind == "drop") {
        Player* link = GET_PLAYER(gPlayState);
        const float dz = c.kind == "chest" ? 150.0f : -150.0f;
        const Vec3f at = { link->actor.world.pos.x, link->actor.world.pos.y, link->actor.world.pos.z + dz };
        const Actor* a = Enemies::TestSpawnAt(c.kind == "chest" ? ACTOR_EN_BOX : ACTOR_EN_ITEM00, at,
                                              static_cast<s16>(0x8000), static_cast<uint16_t>(c.value));
        SPDLOG_INFO("[Coop] TEST: {} {} params 0x{:04X} at ({:.0f}, {:.0f}, {:.0f})",
                    a != nullptr ? "spawned" : "could not spawn", c.kind == "chest" ? "En_Box" : "En_Item00", c.value,
                    at.x, at.y, at.z);
    } else if (c.kind == "open" || c.kind == "pick") {
        const s16 id = c.kind == "open" ? ACTOR_EN_BOX : ACTOR_EN_ITEM00;
        Player* link = GET_PLAYER(gPlayState);
        Actor* nearest = nullptr;
        for (int category = 0; category < ACTORCAT_MAX; category++) {
            for (Actor* a = gPlayState->actorCtx.actorLists[category].first; a != nullptr; a = a->next) {
                // Chests already opened (their treasure flag, params & 0x1F) do not count.
                if (a->id == id && a->update != nullptr &&
                    !(id == ACTOR_EN_BOX && Flags_GetTreasure(gPlayState, a->params & 0x1F)) &&
                    (nearest == nullptr ||
                     Actor_WorldDistXZToActor(&link->actor, a) < Actor_WorldDistXZToActor(&link->actor, nearest))) {
                    nearest = a;
                }
            }
        }
        SPDLOG_INFO("[Coop] TEST: change {}: {} params 0x{:04X}; Link state1 0x{:08X} state2 0x{:08X}, message mode {}",
                    c.kind, nearest != nullptr ? "nearest" : "nothing to take, no actor", nearest ? nearest->params : 0,
                    link->stateFlags1, link->stateFlags2, gPlayState->msgCtx.msgMode);
        if (nearest != nullptr) {
            // A chest opens for a Link 0-50 units behind its facing (EnBox_WaitOpen), turned towards it; a collectible
            // is picked up by touching it.
            const s16 yaw = nearest->shape.rot.y;
            const float d = c.kind == "open" ? -30.0f : 0.0f;
            const Vec3f pos = { nearest->world.pos.x + d * Math_SinS(yaw), nearest->world.pos.y,
                                nearest->world.pos.z + d * Math_CosS(yaw) };
            link->actor.world.pos = link->actor.prevPos = link->actor.home.pos = pos;
            link->actor.shape.rot.y = link->actor.world.rot.y = link->yaw = yaw;
            if (c.kind == "open") {
                sOpenPresses = 15;
                sOpenNext = SteadyClock::now() + std::chrono::milliseconds(150);
            }
        }
    }
    SPDLOG_INFO("[Coop] TEST: change {} 0x{:X} (rupees {}, accumulator {})", c.kind, c.value,
                gSaveContext.save.saveInfo.playerData.rupees, gSaveContext.rupeeAccumulator);
}

void PressStart() {
    Input* input = CONTROLLER1(&gPlayState->state);
    input->cur.button |= BTN_START;
    input->press.button |= BTN_START;
}

void UpdateHolds() {
    const auto& holds = GetLaunchOptions().testHold;
    PlayState* play = gPlayState;
    Player* link = GET_PLAYER(play);
    const auto now = SteadyClock::now();
    if (!sHold.active) {
        if (sNextHold < holds.size() && GameplaySeconds() >= holds[sNextHold].atSeconds && NoTransitionInProgress()) {
            const LaunchOptions::TimedHold& h = holds[sNextHold++];
            sHold = { true, h.kind, now + std::chrono::milliseconds(h.ms), now, false };
            SPDLOG_INFO("[Coop] TEST: hold {} for {} ms", h.kind, h.ms);
            if (h.kind == "pause") {
                PressStart();
            } else if (h.kind == "text") {
                Message_StartTextbox(play, 0x5E6, nullptr);
            } else if (h.kind == "cutscene") {
                play->tryPlayerCsAction(play, nullptr, PLAYER_CSACTION_WAIT);
            }
        }
        return;
    }
    if (sHold.kind == "knock" && now >= sHold.nextKnock && now < sHold.until) {
        sHold.nextKnock = now + std::chrono::milliseconds(400);
        // A knockback request, as an enemy's attack makes one (z_actor.c, func_800B8D10); a weak one, each time the
        // other way, so the Link reels on the spot instead of being pushed out of the room.
        static bool sFlip = false;
        sFlip = !sFlip;
        func_800B8D10(play, &link->actor, 1.0f, static_cast<s16>(link->actor.shape.rot.y + (sFlip ? 0x8000 : 0)), 2.0f,
                      2, 0);
    }
    if (now < sHold.until) {
        return;
    }
    if (sHold.kind == "pause") {
        // START again once the menu is fully open; until it is closed.
        if (IS_PAUSED(&play->pauseCtx)) {
            if (play->pauseCtx.state == PAUSE_STATE_MAIN) {
                PressStart();
            }
            return;
        }
    } else if (sHold.kind == "text") {
        if (play->msgCtx.msgMode != MSGMODE_NONE) {
            // Closed once, as the game closes a text box (each call would restart the closing timer); if it is still
            // open 3 s later the test leaves the message system idle.
            if (!sHold.closing) {
                sHold.closing = true;
                Message_CloseTextbox(play);
            } else if (now >= sHold.until + std::chrono::seconds(3)) {
                SPDLOG_INFO("[Coop] TEST: hold text: forced closed (msgMode {})",
                            static_cast<int>(play->msgCtx.msgMode));
                play->msgCtx.msgMode = MSGMODE_NONE;
            }
            return;
        }
    } else if (sHold.kind == "cutscene") {
        if (link->csAction != PLAYER_CSACTION_NONE && link->csAction != PLAYER_CSACTION_END) {
            // Player_TryCsAction refuses a new action while one is running; the Link's own cutscene action handles
            // END (back to normal play) when it is set directly, as cutscene actors do.
            link->csAction = PLAYER_CSACTION_END;
            return;
        }
        if (link->csAction != PLAYER_CSACTION_NONE) {
            return; // Ending
        }
    }
    SPDLOG_INFO("[Coop] TEST: hold {} over", sHold.kind);
    sHold.active = false;
}

// --coop-test-cross: the Link in front of a transition actor (door or room plane), facing it, on the side of its
// current room; then A for a door or the stick held for a room plane.
void StartCross(int index) {
    PlayState* play = gPlayState;
    Player* player = GET_PLAYER(play);
    if (index >= play->transitionActors.count) {
        SPDLOG_INFO("[Coop] TEST: cross {}: the scene has {} transitions", index, play->transitionActors.count);
        return;
    }
    const TransitionActorEntry& t = play->transitionActors.list[index];
    const int16_t id = static_cast<int16_t>(t.id < 0 ? -t.id : t.id) & 0x1FFF;
    const bool door =
        id == ACTOR_EN_DOOR || id == ACTOR_DOOR_SHUTTER || id == ACTOR_DOOR_SPIRAL || id == ACTOR_BG_OPEN_SHUTTER;
    const s16 yaw = DEG_TO_BINANG((t.rotY >> 7) & 0x1FF);
    const int8_t cur = play->roomCtx.curRoom.num;
    // sides[1] is the room the transition faces (in Woodfall, room 2 lies along the facing of door 2, rooms 1/2).
    const bool inFront = t.sides[1].room == cur;
    // Door_Shutter notices the player under 50 along its normal. A room plane (En_Holl; not the vertical kinds) is
    // walked across from out of its range (200 for the visible kind), as a player would, to out of it on the other
    // side (where the old room is dropped).
    const float distance = door ? 30.0f : 250.0f;
    const float sign = inFront ? 1.0f : -1.0f;
    const Vec3f pos = { t.pos.x + Math_SinS(yaw) * distance * sign, t.pos.y + 20.0f,
                        t.pos.z + Math_CosS(yaw) * distance * sign };
    const s16 facing = static_cast<s16>(inFront ? yaw + 0x8000 : yaw);
    // The floor right there (a door can sit on a ledge above a pit on one side).
    Vec3f probe = { pos.x, pos.y + 50.0f, pos.z };
    CollisionPoly* poly = nullptr;
    s32 bgId = 0;
    const f32 floorY = BgCheck_EntityRaycastFloor3(&play->colCtx, &poly, &bgId, &probe);
    // Off any moving floor first (a door's own collision is one): the game would carry the Link back on it.
    player->actor.floorBgId = BGCHECK_SCENE;
    // home.pos too: Player checks its move against walls from there (it is its previous position); without it a long
    // jump stops at the first wall in between.
    player->actor.world.pos = pos;
    player->actor.prevPos = pos;
    player->actor.home.pos = pos;
    player->actor.shape.rot.y = player->actor.world.rot.y = player->yaw = facing;
    player->speedXZ = 0.0f;
    sStickYaw = facing;
    const auto now = SteadyClock::now();
    sCross = { true, index,  cur, door ? 3 : 0, now + std::chrono::milliseconds(400), now + std::chrono::seconds(6),
               pos,  facing, 0 };
    if (!door) {
        sWalkUntil = now + std::chrono::milliseconds(3500);
    }
    SPDLOG_INFO("[Coop] TEST: crossing transition {} ({}, rooms {}/{}) from room {} at ({:.0f}, {:.0f}, {:.0f}); floor "
                "{:.0f}",
                index, Enemies::TypeName(id), t.sides[0].room, t.sides[1].room, cur, pos.x, pos.y, pos.z, floorY);
}

void UpdateCross() {
    const auto& steps = GetLaunchOptions().testCross;
    const auto now = SteadyClock::now();
    // A door that does not open can answer the test's A presses with a message: closed when the attempt ends, as a
    // player would (once, as the game closes a text box; the message system left idle if it is still open 3 s later).
    static SteadyClock::time_point sClosingTextSince = {};
    if (sClosingTextSince != SteadyClock::time_point{}) {
        if (gPlayState->msgCtx.msgMode == MSGMODE_NONE) {
            sClosingTextSince = {};
        } else if (now - sClosingTextSince > std::chrono::seconds(3)) {
            SPDLOG_INFO("[Coop] TEST: cross: message forced closed (msgMode {})",
                        static_cast<int>(gPlayState->msgCtx.msgMode));
            gPlayState->msgCtx.msgMode = MSGMODE_NONE;
            sClosingTextSince = {};
        }
    }
    if (sCross.active) {
        // The game sometimes puts the Link back where it stood on the next frame; place it again (before any press).
        Player* link = GET_PLAYER(gPlayState);
        const float dx = link->actor.world.pos.x - sCross.placed.x, dz = link->actor.world.pos.z - sCross.placed.z;
        if (sCross.pressesLeft == 3 && sCross.replaced < 3 && dx * dx + dz * dz > 20.0f * 20.0f) {
            sCross.replaced++;
            link->actor.floorBgId = BGCHECK_SCENE;
            link->actor.world.pos = sCross.placed;
            link->actor.prevPos = sCross.placed;
            link->actor.home.pos = sCross.placed;
            link->actor.shape.rot.y = link->actor.world.rot.y = link->yaw = sCross.facing;
            link->speedXZ = 0.0f;
            sCross.nextPress = now + std::chrono::milliseconds(400);
            SPDLOG_INFO("[Coop] TEST: cross {}: Link placed again ({})", sCross.index, sCross.replaced);
        }
        if (sCross.pressesLeft > 0 && now >= sCross.nextPress && sHeldButtons == 0) {
            sHeldButtons = BTN_A;
            sHeldStick = false;
            sPressEdge = true;
            sHeldUntil = now + std::chrono::milliseconds(100);
            sCross.pressesLeft--;
            sCross.nextPress = now + std::chrono::milliseconds(700);
        }
        // Done as soon as the room changed and the door (a cutscene) is over, so tests can cross quickly. A room plane
        // changes rooms before the Link is through it: only after the walk there.
        const int8_t current = gPlayState->roomCtx.curRoom.num;
        if (current != sCross.fromRoom && current >= 0 && now >= sWalkUntil &&
            GET_PLAYER(gPlayState)->csAction == PLAYER_CSACTION_NONE && !Play_InCsMode(gPlayState) &&
            sCross.reportAt - now > std::chrono::milliseconds(300)) {
            sCross.reportAt = now + std::chrono::milliseconds(300);
        }
        if (now >= sCross.reportAt) {
            const int8_t room = gPlayState->roomCtx.curRoom.num;
            sCross.active = false;
            if (gPlayState->msgCtx.msgMode != MSGMODE_NONE && sClosingTextSince == SteadyClock::time_point{}) {
                SPDLOG_INFO("[Coop] TEST: cross {}: closing the message the door showed (text 0x{:X})", sCross.index,
                            gPlayState->msgCtx.currentTextId);
                Message_CloseTextbox(gPlayState);
                sClosingTextSince = now;
            }
            if (room == sCross.fromRoom && sCrossRetries < 2) {
                // The test placement did not hold (the game moved the Link away); try the same door again.
                sCrossRetries++;
                sRetryAt = now + std::chrono::seconds(3);
                SPDLOG_INFO("[Coop] TEST: cross {}: still in room {}; retry {} in 3 s", sCross.index, room,
                            sCrossRetries);
                return;
            }
            SPDLOG_INFO("[Coop] TEST: cross {}: room {} -> {} ({} retries)", sCross.index, sCross.fromRoom, room,
                        sCrossRetries);
            sCrossRetries = 0;
            sNextCross++;
        }
        return;
    }
    // A short walk 2 s before: right after a door the Link stands on the door's own collision, and the game puts it
    // back there when it is moved away; a few steps off it first.
    static size_t sNudged = SIZE_MAX;
    const int64_t unixMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    auto dueIn = [&](const LaunchOptions::TimedValue& step, int64_t aheadMs) {
        return step.atUnixMs > 0 ? unixMs + aheadMs >= step.atUnixMs
                                 : (GameplaySeconds() + aheadMs / 1000) >= step.atSeconds;
    };
    if (sNextCross < steps.size() && sNudged != sNextCross && dueIn(steps[sNextCross], 2000) &&
        NoTransitionInProgress() && !Rooms::MoveInProgress()) {
        sNudged = sNextCross;
        sStickYaw = GET_PLAYER(gPlayState)->actor.shape.rot.y;
        sWalkUntil = now + std::chrono::milliseconds(800);
    }
    if (sNextCross < steps.size() && dueIn(steps[sNextCross], 0) && NoTransitionInProgress() &&
        !Rooms::MoveInProgress() && now >= sWalkUntil && now >= sRetryAt) {
        StartCross(steps[sNextCross].value);
    }
}

// --coop-test-hurt / --coop-test-health / --coop-test-give-fairy / --coop-test-log-health (player damage tests).
size_t sNextHurt = 0;
size_t sNextHealth = 0;
bool sFairyGiven = false;
int16_t sLoggedHealth = -1;
std::string sLoggedBottles;

void UpdateHealthTests() {
    const LaunchOptions& options = GetLaunchOptions();
    PlayState* play = gPlayState;
    s16& health = gSaveContext.save.saveInfo.playerData.health;
    // Logged before --coop-test-keep-health fills it again.
    if (options.testLogHealth || options.testKeepHealth) {
        if (sLoggedHealth >= 0 && health != sLoggedHealth) {
            SPDLOG_INFO("[Coop] TEST: health {} -> {}", sLoggedHealth, health);
        }
        sLoggedHealth = health;
    }
    if (options.testLogHealth) {
        std::string bottles;
        for (int slot = SLOT_BOTTLE_1; slot <= SLOT_BOTTLE_6; slot++) {
            bottles += fmt::format("{}0x{:02X}", slot == SLOT_BOTTLE_1 ? "" : " ",
                                   gSaveContext.save.saveInfo.inventory.items[slot]);
        }
        if (bottles != sLoggedBottles) {
            SPDLOG_INFO("[Coop] TEST: bottles {} (fairy is 0x{:02X}, empty bottle 0x{:02X})", bottles,
                        static_cast<int>(ITEM_FAIRY), static_cast<int>(ITEM_BOTTLE));
            sLoggedBottles = bottles;
        }
    }
    if (!NoTransitionInProgress() || GET_PLAYER(play) == nullptr) {
        return;
    }
    const int seconds = GameplaySeconds();
    if (sNextHurt < options.testHurt.size() && seconds >= options.testHurt[sNextHurt].atSeconds) {
        const int damage = options.testHurt[sNextHurt++].value;
        SPDLOG_INFO("[Coop] TEST: damagePlayer {} on the local Link (health {})", -damage, health);
        play->damagePlayer(play, -damage);
    }
    if (sNextHealth < options.testHealth.size() && seconds >= options.testHealth[sNextHealth].atSeconds) {
        const int value = options.testHealth[sNextHealth++].value;
        SPDLOG_INFO("[Coop] TEST: health set {} -> {}", health, value);
        health = static_cast<s16>(std::min<int>(value, gSaveContext.save.saveInfo.playerData.healthCapacity));
        sLoggedHealth = health;
    }
    if (options.testGiveFairyAt.has_value() && !sFairyGiven && seconds >= *options.testGiveFairyAt) {
        sFairyGiven = true;
        gSaveContext.save.saveInfo.inventory.items[SLOT_BOTTLE_1] = ITEM_FAIRY;
        SPDLOG_INFO("[Coop] TEST: a fairy in the first bottle slot (in memory)");
    }
}

// --coop-test-log-transitions: the scene's rooms and transition actors (doors, room planes), to pick test routes.
void OnRoomInitLogTransitions(s8 sceneId, s8 roomNum) {
    if (!GetLaunchOptions().testLogTransitions || gPlayState == nullptr) {
        return;
    }
    PlayState* play = gPlayState;
    SPDLOG_INFO("[Coop] TEST: transitions of scene {} (room {} of {}): {}", sceneId, roomNum, play->roomList.count,
                play->transitionActors.count);
    for (int i = 0; i < play->transitionActors.count; i++) {
        const TransitionActorEntry& t = play->transitionActors.list[i];
        const int16_t id = static_cast<int16_t>(t.id < 0 ? -t.id : t.id) & 0x1FFF;
        SPDLOG_INFO("[Coop] TEST: transition {}: {} rooms {}/{} pos ({}, {}, {}) rotY 0x{:04X} params 0x{:04X}", i,
                    id < ACTOR_ID_MAX ? Enemies::TypeName(id) : "?", t.sides[0].room, t.sides[1].room, t.pos.x, t.pos.y,
                    t.pos.z, static_cast<uint16_t>(t.rotY), t.params);
    }
}

// --coop-test-log-transitions also lists the enemies of the room 1 s after each room change, and whether the room is
// marked cleared (doors that wait for it).
void LogRoomEnemies() {
    static int8_t sRoom = -1;
    static SteadyClock::time_point sDueAt = {};
    PlayState* play = gPlayState;
    if (!GetLaunchOptions().testLogTransitions) {
        return;
    }
    const auto now = SteadyClock::now();
    const int8_t room = play->roomCtx.curRoom.num;
    if (room != sRoom) {
        sRoom = room;
        sDueAt = now + std::chrono::seconds(1);
        return;
    }
    if (room < 0 || sDueAt == SteadyClock::time_point{} || now < sDueAt) {
        return;
    }
    sDueAt = {};
    std::string list;
    int count = 0;
    for (Actor* actor = play->actorCtx.actorLists[ACTORCAT_ENEMY].first; actor != nullptr; actor = actor->next) {
        list += fmt::format("{}{} 0x{:04X} room {}", count == 0 ? "" : ", ", Enemies::TypeName(actor->id),
                            static_cast<uint16_t>(actor->params), actor->room);
        count++;
    }
    SPDLOG_INFO("[Coop] TEST: room {} enemies {} (cleared {}, cleared for now {}): {}", room, count,
                Flags_GetClear(play, room) ? 1 : 0, Flags_GetClearTemp(play, room) ? 1 : 0, list);
}

void RegisterTestHooks() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnRoomInit>(OnRoomInitLogTransitions);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPassPlayerInputs>(OnPassPlayerInputs);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnConsoleLogoUpdate>(OnConsoleLogoUpdate);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameStateMainFinish>(OnGameStateMainFinish);
    // --coop-test-log-transitions also names the actor cutscenes that start (a room's entry cutscene holds the Link).
    REGISTER_VB_SHOULD(VB_START_CUTSCENE, {
        if (!GetLaunchOptions().testLogTransitions || gPlayState == nullptr || !*should) {
            return;
        }
        const s16 csId = *va_arg(args, s16*);
        Actor* actor = va_arg(args, Actor*);
        if (csId > CS_ID_NONE && CutsceneManager_GetCurrentCsId() == CS_ID_NONE) {
            SPDLOG_INFO("[Coop] TEST: cutscene {} starts ({} params 0x{:04X} in room {}; Link in room {})", csId,
                        actor != nullptr ? Enemies::TypeName(actor->id) : "no actor",
                        actor != nullptr ? static_cast<uint16_t>(actor->params) : 0,
                        actor != nullptr ? actor->room : -1, gPlayState->roomCtx.curRoom.num);
        }
    });
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
    UpdateEnemyTests();
    UpdateAttacks();
    UpdateShots();
    // --coop-test-hit: the nearest enemy hit with given damage flags (enemy health phase tests).
    const auto& hits = GetLaunchOptions().testHit;
    sHits.resize(hits.size());
    for (size_t i = 0; i < hits.size(); i++) {
        AttackState& state = sHits[i];
        if (GameplaySeconds() < hits[i].atSeconds || state.done >= hits[i].count || SteadyClock::now() < state.next) {
            continue;
        }
        state.done++;
        state.next = SteadyClock::now() + std::chrono::milliseconds(hits[i].intervalMs);
        Combat::TestHitNearest(static_cast<uint32_t>(std::strtoul(hits[i].button.c_str(), nullptr, 16)));
        break;
    }
    // --coop-test-near: the local Link at a distance from the nearest enemy (enemy targeting tests).
    const auto& nearSteps = GetLaunchOptions().testNear;
    if (sNextNear < nearSteps.size() && GameplaySeconds() >= nearSteps[sNextNear].atSeconds &&
        NoTransitionInProgress()) {
        ApproachNearestEnemy(nearSteps[sNextNear++].value);
    }
    UpdateHealthTests();
    UpdateCross();
    UpdateHolds();
    UpdateChanges();
    LogRoomEnemies();
    // --coop-test-room: the host taken to another room by the room move itself.
    static size_t sNextRoom = 0;
    const auto& rooms = GetLaunchOptions().testRoom;
    const int64_t wallMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    if (sNextRoom < rooms.size() &&
        (rooms[sNextRoom].atUnixMs > 0 ? wallMs >= rooms[sNextRoom].atUnixMs
                                       : GameplaySeconds() >= rooms[sNextRoom].atSeconds) &&
        NoTransitionInProgress() && !Rooms::MoveInProgress()) {
        const LaunchOptions::TimedRoom& r = rooms[sNextRoom++];
        Rooms::TestMoveTo(static_cast<int8_t>(r.room), { r.x, r.y, r.z });
    }
    // --coop-test-visit-rooms: every room of the scene in turn, 120 units inside the first door or room plane that
    // leads into it, on the floor there.
    static int sVisitRoom = 0;
    static SteadyClock::time_point sVisitNext = {};
    const std::optional<int>& visitAt = GetLaunchOptions().testVisitRoomsAt;
    if (visitAt.has_value() && GameplaySeconds() >= *visitAt && sVisitRoom < gPlayState->roomList.count &&
        SteadyClock::now() >= sVisitNext && NoTransitionInProgress() && !Rooms::MoveInProgress()) {
        PlayState* play = gPlayState;
        const int room = sVisitRoom++;
        sVisitNext = SteadyClock::now() + std::chrono::seconds(8);
        bool found = false;
        for (int i = 0; i < play->transitionActors.count && !found; i++) {
            const TransitionActorEntry& t = play->transitionActors.list[i];
            if (t.sides[0].room == t.sides[1].room || (t.sides[0].room != room && t.sides[1].room != room)) {
                continue;
            }
            const s16 yaw = DEG_TO_BINANG((t.rotY >> 7) & 0x1FF);
            const float sign = t.sides[1].room == room ? 1.0f : -1.0f;
            Vec3f probe = { t.pos.x + Math_SinS(yaw) * 120.0f * sign, t.pos.y + 60.0f,
                            t.pos.z + Math_CosS(yaw) * 120.0f * sign };
            CollisionPoly* poly = nullptr;
            s32 bgId = 0;
            const f32 floorY = BgCheck_EntityRaycastFloor3(&play->colCtx, &poly, &bgId, &probe);
            if (poly != nullptr && floorY > t.pos.y - 100.0f) {
                found = true;
                SPDLOG_INFO("[Coop] TEST: visiting room {} by transition {} at ({:.0f}, {:.0f}, {:.0f})", room, i,
                            probe.x, floorY, probe.z);
                Rooms::TestMoveTo(static_cast<int8_t>(room), { probe.x, floorY, probe.z });
            }
        }
        if (!found) {
            SPDLOG_INFO("[Coop] TEST: room {} has no door with a floor inside to arrive by", room);
        }
    }
    // --coop-test-keep-health: the host's Link stands still among the test enemies, which really attack it.
    if (GetLaunchOptions().testKeepHealth) {
        gSaveContext.save.saveInfo.playerData.health = gSaveContext.save.saveInfo.playerData.healthCapacity;
    }
}

static RegisterShipInitFunc initFunc(RegisterTestHooks);

} // namespace Coop::Test
