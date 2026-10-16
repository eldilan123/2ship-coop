#include "CoopSession.h"

#include <chrono>
#include <cstring>
#include <optional>
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include "Coop.h"
#include "CoopArgs.h"
#include "CoopClock.h"
#include "CoopPlayerSync.h"
#include "CoopSaveGuard.h"
#include "CoopSnapshot.h"
#include "2s2h/BenGui/Notification.h"
#include "2s2h/BenGui/BenGui.hpp"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "variables.h"
#include "functions.h"
#include "macros.h"
#include "overlays/gamestates/ovl_title/z_title.h"
extern SceneEntranceTableEntry sSceneEntranceTable[];
}

namespace Coop {

namespace {

// MARK: - Client state

enum class ClientStage {
    None,      // Not in a session
    Requested, // "Enter session" sent, waiting for the join snapshot
    Entering,  // Snapshot applied, switching to Play
    InSession, // Playing on the host's world
    Leaving,   // Session lost: going back to the title screen; saving stays blocked until we get there
};

ClientStage sStage = ClientStage::None;
uint32_t sLastSnapshotSeq = 0;
Snapshot::Header sLastHost = {}; // Header of the last applied snapshot
uint8_t sEnterWeekEventReg92 = 0;

// Scene the client has to move to, from the newest snapshot. Only one is kept.
struct PendingDestination {
    bool active;
    int32_t entrance;
    int32_t sceneLayer;
    int16_t sceneId;
    uint32_t seq;
    bool force; // Reload even if already in that scene (releases a held exit)
    std::chrono::steady_clock::time_point since;
};
PendingDestination sPending = {};
void ArriveNextToHost(uint16_t entrance, int16_t sceneId);
bool sFollowInProgress = false; // Our own transition to the host's scene; never intercepted

// Exit the client walked into, waiting for the host to take it (scene change part 2).
using SteadyClock = std::chrono::steady_clock;
constexpr auto kExitRequestTimeout = std::chrono::seconds(3);
struct HeldExit {
    bool active;
    uint16_t entrance;
    uint16_t nextCutsceneIndex;
    SteadyClock::time_point since;
};
HeldExit sHeldExit = {};

// Entrances from the network index the scene entrance tables; reject anything outside them.
bool IsValidEntrance(uint16_t entrance) {
    const uint32_t scene = entrance >> 9;
    const uint32_t spawn = (entrance >> 4) & 0x1F;
    return scene < ENTR_SCENE_MAX && sSceneEntranceTable[scene].table != nullptr &&
           spawn < sSceneEntranceTable[scene].tableCount && (entrance & 0xF) == 0;
}

// MARK: - Host state

uint32_t sNextSnapshotSeq = 1;
std::optional<u16> sHostResetCount; // threeDayResetCount when the last scene snapshot was sent

bool InGameplay() {
    return gPlayState != nullptr && gSaveContext.gameMode == GAMEMODE_NORMAL;
}

void Notify(const std::string& message) {
    Notification::Emit({ .message = message });
}

// MARK: - Host

void HostSendSnapshot(Snapshot::Reason reason, uint32_t target) {
    const uint32_t seq = sNextSnapshotSeq++;
    const std::vector<uint8_t> bytes = Snapshot::Build(reason, seq);
    SendBinary(target, Protocol::kChannelSnapshot, bytes.data(), bytes.size());
    SPDLOG_INFO("[Coop] Sent snapshot #{} ({}) to {}: scene {} entrance 0x{:04X} room {}, {} bytes", seq,
                Snapshot::ReasonName(static_cast<uint16_t>(reason)),
                target == kBroadcast ? std::string("all") : fmt::format("player {}", target), gPlayState->sceneId,
                static_cast<uint16_t>(gSaveContext.save.entrance), gPlayState->roomCtx.curRoom.num, bytes.size());
}

void HostOnEnterRequest(uint32_t from, const nlohmann::json&) {
    if (!InGameplay()) {
        SPDLOG_INFO("[Coop] Player {} asked to enter, but the host is not in gameplay yet", from);
        SendJson(from, { { "type", "enter_denied" }, { "reason", "The host is not in gameplay yet. Try again." } });
        return;
    }
    HostSendSnapshot(Snapshot::Reason::Join, from);
}

// MARK: - Client

void ClientOnEnterDenied(uint32_t, const nlohmann::json& message) {
    if (sStage != ClientStage::Requested) {
        return;
    }
    const std::string reason = Protocol::GetString(message, "reason", "The host refused.");
    SPDLOG_INFO("[Coop] Enter session refused: {}", reason);
    Notify("Coop: " + reason);
    sStage = ClientStage::None;
}

// Same preparation FileSelect_LoadGame does after Sram_OpenSave, but on the in-memory snapshot. Sram_OpenSave itself
// is not called: it reads the client's own file and would erase its owl save.
void ClientEnterGameplay(const Snapshot::Parsed& snapshot) {
    Snapshot::ApplyShared(snapshot, true);

    // A valid slot index keeps the flash page tables in bounds; every write is blocked by the session flag anyway.
    gSaveContext.fileNum = 0;
    gSaveContext.gameMode = GAMEMODE_NORMAL;
    gSaveContext.save.saveInfo.playerData.magicLevel = 0;
    gSaveContext.save.entrance = snapshot.header.entrance;
    // Arrive on the host's scene layer without replaying a cutscene trigger.
    gSaveContext.save.cutsceneIndex = snapshot.header.sceneLayer > 0 ? 0xFFF0 + snapshot.header.sceneLayer - 1 : 0;
    gSaveContext.nextCutsceneIndex = 0xFFEF;

    gSaveContext.respawnFlag = 0;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].entrance = ENTR_LOAD_OPENING;
    gSaveContext.seqId = NA_BGM_DISABLED;
    gSaveContext.ambienceId = AMBIENCE_ID_DISABLED;
    gSaveContext.showTitleCard = true;
    gSaveContext.dogParams = 0;
    for (int i = 0; i < TIMER_ID_MAX; i++) {
        gSaveContext.timerStates[i] = TIMER_STATE_OFF;
    }
    gSaveContext.prevHudVisibility = HUD_VISIBILITY_ALL;
    gSaveContext.nayrusLoveTimer = 0;
    gSaveContext.healthAccumulator = 0;
    gSaveContext.magicFlag = 0;
    gSaveContext.forcedSeqId = 0;
    gSaveContext.skyboxTime = gSaveContext.save.time;
    gSaveContext.nextTransitionType = TRANS_NEXT_TYPE_DEFAULT;
    gSaveContext.cutsceneTrigger = 0;
    gSaveContext.chamberCutsceneNum = 0;
    gSaveContext.nextDayTime = NEXT_TIME_NONE;
    gSaveContext.retainWeatherMode = false;
    gSaveContext.buttonStatus[EQUIP_SLOT_B] = BTN_ENABLED;
    gSaveContext.buttonStatus[EQUIP_SLOT_C_LEFT] = BTN_ENABLED;
    gSaveContext.buttonStatus[EQUIP_SLOT_C_DOWN] = BTN_ENABLED;
    gSaveContext.buttonStatus[EQUIP_SLOT_C_RIGHT] = BTN_ENABLED;
    gSaveContext.buttonStatus[EQUIP_SLOT_A] = BTN_ENABLED;
    gSaveContext.shipSaveContext.dpad.status[EQUIP_SLOT_D_RIGHT] = BTN_ENABLED;
    gSaveContext.shipSaveContext.dpad.status[EQUIP_SLOT_D_LEFT] = BTN_ENABLED;
    gSaveContext.shipSaveContext.dpad.status[EQUIP_SLOT_D_DOWN] = BTN_ENABLED;
    gSaveContext.shipSaveContext.dpad.status[EQUIP_SLOT_D_UP] = BTN_ENABLED;
    gSaveContext.hudVisibilityForceButtonAlphasByStatus = false;
    gSaveContext.nextHudVisibility = HUD_VISIBILITY_IDLE;
    gSaveContext.hudVisibility = HUD_VISIBILITY_IDLE;
    gSaveContext.hudVisibilityTimer = 0;
    gSaveContext.save.saveInfo.playerData.tatlTimer = 0;

    // Play_Destroy (if we are leaving the title screen) clears this flag after we return; restore it there.
    sEnterWeekEventReg92 = gSaveContext.save.saveInfo.weekEventReg[92];

    SetClientInSession(true, "entering the host's session");
    GameInteractor_ExecuteOnSaveLoad(gSaveContext.fileNum);
    ArriveNextToHost(static_cast<uint16_t>(snapshot.header.entrance), snapshot.header.sceneId);

    STOP_GAMESTATE(gGameState);
    SET_NEXT_GAMESTATE(gGameState, Play_Init, sizeof(PlayState));
    sStage = ClientStage::Entering;
    SPDLOG_INFO("[Coop] Entering the host's session without a save slot: entrance 0x{:04X}, layer {}",
                static_cast<uint16_t>(snapshot.header.entrance), snapshot.header.sceneLayer);
}

void ClientOnSnapshot(uint32_t, const std::vector<uint8_t>& bytes) {
    Snapshot::Parsed snapshot;
    std::string error;
    if (!Snapshot::Parse(bytes, snapshot, error)) {
        SPDLOG_WARN("[Coop] Snapshot rejected: {}", error);
        Notify("Coop: snapshot from the host rejected: " + error);
        if (sStage == ClientStage::Requested) {
            sStage = ClientStage::None;
        }
        return;
    }
    const Snapshot::Header& h = snapshot.header;
    if (h.seq <= sLastSnapshotSeq) {
        SPDLOG_INFO("[Coop] Ignoring old snapshot #{} (last applied #{})", h.seq, sLastSnapshotSeq);
        return;
    }

    if (sStage == ClientStage::Requested) {
        if (gGameState == nullptr) {
            return;
        }
        sLastSnapshotSeq = h.seq;
        sLastHost = h;
        SPDLOG_INFO("[Coop] Received snapshot #{} ({}): host scene {} entrance 0x{:04X} room {}", h.seq,
                    Snapshot::ReasonName(h.reason), h.sceneId, static_cast<uint16_t>(h.entrance), h.roomNum);
        ClientEnterGameplay(snapshot);
        return;
    }
    if (sStage == ClientStage::None) {
        return; // Not in the session; the host broadcasts scene snapshots to everyone.
    }

    sLastSnapshotSeq = h.seq;
    sLastHost = h;
    Snapshot::ApplyShared(snapshot, false);
    SPDLOG_INFO("[Coop] Applied snapshot #{} ({}): host scene {} entrance 0x{:04X} room {}", h.seq,
                Snapshot::ReasonName(h.reason), h.sceneId, static_cast<uint16_t>(h.entrance), h.roomNum);

    // Only the newest destination matters: if the host moved several times before we could follow, go straight to
    // where it is now.
    if (sPending.active) {
        SPDLOG_INFO("[Coop] Replacing pending destination (scene {} from snapshot #{}) with scene {} from #{}",
                    sPending.sceneId, sPending.seq, h.sceneId, h.seq);
    }
    sPending = { true, h.entrance, h.sceneLayer, h.sceneId, h.seq, false, std::chrono::steady_clock::now() };
}

constexpr auto kWaitForHostState = std::chrono::milliseconds(500);

// Rooms (pending since milestone 1): with the host's position known, the client arrives in the host's room and spot
// instead of at the entrance's spawn point, the way WarpPoint places Link (respawn data + respawnFlag -8).
void ArriveNextToHost(uint16_t entrance, int16_t sceneId) {
    PlayerSync::State host;
    if (!PlayerSync::LatestState(kHostPlayerId, host) || host.sceneId != sceneId || !host.inGameplay) {
        return;
    }
    RespawnData& respawn = gSaveContext.respawn[RESPAWN_MODE_DOWN];
    respawn.entrance = entrance;
    respawn.roomIndex = static_cast<u8>(host.roomNum);
    respawn.pos = host.pos;
    respawn.yaw = host.shapeRot.y;
    respawn.playerParams = PLAYER_PARAMS(0xFF, PLAYER_START_MODE_D);
    respawn.tempSwitchFlags = 0;
    respawn.unk_18 = 0;
    respawn.tempCollectFlags = 0;
    gSaveContext.respawnFlag = -8;
    SPDLOG_INFO("[Coop] Arriving next to the host: scene {} room {} pos ({:.0f}, {:.0f}, {:.0f})", sceneId,
                host.roomNum, host.pos.x, host.pos.y, host.pos.z);
}

// Same as the debug console "entrance" command: let Play's own transition code load the host's scene.
void ClientFollowHost(const PendingDestination& dest) {
    const uint16_t entrance = static_cast<uint16_t>(dest.entrance);
    gPlayState->nextEntrance = entrance & ~0xF;
    ArriveNextToHost(gPlayState->nextEntrance, dest.sceneId);
    // A scene layer is selected through the cutscene index, as cutscene destinations do (see z_demo.c).
    gSaveContext.nextCutsceneIndex = dest.sceneLayer > 0 ? 0xFFEF + dest.sceneLayer : 0xFFEF;
    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
    gPlayState->transitionType = TRANS_TYPE_FADE_BLACK;
    gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK;
    SPDLOG_INFO("[Coop] Following the host from scene {} to scene {} (entrance 0x{:04X}, layer {})",
                gPlayState->sceneId, dest.sceneId, entrance, dest.sceneLayer);
}

// Adjustment 1: the Song of Time and owl statues would start the client's own cycle reset or save and split it from
// the host. They are caught at the start of the frame, before Message_Update acts on them, and cancelled the same way
// answering "No" does.
void ClientBlockHostOnlyActions() {
    MessageContext* msgCtx = &gPlayState->msgCtx;
    const char* what = nullptr;
    if (msgCtx->msgMode == MSGMODE_NEW_CYCLE_0) {
        what = "Song of Time";
        msgCtx->ocarinaMode = OCARINA_MODE_END;
    } else if (msgCtx->msgMode == MSGMODE_OWL_SAVE_0) {
        what = "Owl statue save";
    } else {
        return;
    }
    if (msgCtx->msgLength != 0) {
        Message_CloseTextbox(gPlayState);
    } else {
        msgCtx->msgMode = MSGMODE_NONE;
    }
    SPDLOG_WARN("[Coop] Blocked on the client: {}. Only the host can reset the cycle or save.", what);
    Notify(fmt::format("Coop: {} is disabled for players who joined. Only the host can do it.", what));
}

void ClientUpdateSceneFollow() {
    if (!sPending.active || !InGameplay()) {
        return;
    }
    // Wait for any transition in progress (ours or the arrival fade) to finish.
    if (gPlayState->transitionTrigger != TRANS_TRIGGER_OFF || gPlayState->transitionMode != TRANS_MODE_OFF) {
        return;
    }
    // The scene snapshot arrives just before the host's first state in that scene; give it a moment so we can
    // arrive in the host's room, next to it (see ClientFollowHost).
    PlayerSync::State host;
    const bool hostThere = PlayerSync::LatestState(kHostPlayerId, host) && host.sceneId == sPending.sceneId;
    if (!hostThere && std::chrono::steady_clock::now() - sPending.since < kWaitForHostState) {
        return;
    }
    const PendingDestination dest = sPending;
    sPending.active = false;
    if (sHeldExit.active) {
        if ((static_cast<uint16_t>(dest.entrance) & ~0xF) == sHeldExit.entrance) {
            SPDLOG_INFO("[Coop] Held exit to entrance 0x{:04X}: the host took it", sHeldExit.entrance);
        } else {
            SPDLOG_INFO("[Coop] Held exit to entrance 0x{:04X} dropped: the host moved to scene {} first",
                        sHeldExit.entrance, dest.sceneId);
        }
        sHeldExit.active = false;
    }
    if (gPlayState->sceneId == dest.sceneId && !dest.force) {
        return;
    }
    sFollowInProgress = true;
    ClientFollowHost(dest);
}

// Scene change, part 2: when the client walks into a door or loading zone that leads to another scene, the exit is
// held (the player is already frozen by the exit code) and the host is asked to take it. The host's scene snapshot
// then moves everyone. Respawns (void out, death, grottos returning) and same-scene changes stay local.
void ClientInterceptExit() {
    if (sHeldExit.active) {
        // The exit code may set the trigger again while the player stands on the exit; keep it from starting.
        if (gPlayState->transitionTrigger == TRANS_TRIGGER_START && gPlayState->transitionMode == TRANS_MODE_OFF) {
            gPlayState->transitionTrigger = TRANS_TRIGGER_OFF;
        }
        if (SteadyClock::now() - sHeldExit.since > kExitRequestTimeout) {
            sHeldExit.active = false;
            SPDLOG_WARN("[Coop] The host did not take the exit to entrance 0x{:04X}; going back to the host's scene",
                        sHeldExit.entrance);
            Notify("Coop: the host could not take that exit right now.");
            // Reloading the host's entrance also releases the player frozen by the held exit.
            sPending = { true,
                         sLastHost.entrance,
                         sLastHost.sceneLayer,
                         sLastHost.sceneId,
                         sLastHost.seq,
                         true,
                         std::chrono::steady_clock::now() };
        }
        return;
    }
    if (sFollowInProgress || gPlayState->transitionTrigger != TRANS_TRIGGER_START ||
        gPlayState->transitionMode != TRANS_MODE_OFF || gSaveContext.respawnFlag != 0 ||
        gPlayState->gameOverCtx.state != GAMEOVER_INACTIVE) {
        return;
    }
    const uint16_t entrance = gPlayState->nextEntrance;
    if (!IsValidEntrance(entrance) || Entrance_GetSceneIdAbsolute(entrance) == gPlayState->sceneId) {
        return;
    }
    gPlayState->transitionTrigger = TRANS_TRIGGER_OFF;
    sHeldExit = { true, entrance, gSaveContext.nextCutsceneIndex, SteadyClock::now() };
    SendJson(kHostPlayerId,
             { { "type", "scene_request" }, { "entrance", entrance }, { "cutscene", gSaveContext.nextCutsceneIndex } });
    SPDLOG_INFO("[Coop] Exit to entrance 0x{:04X} (scene {}) held; asking the host to take it", entrance,
                Entrance_GetSceneIdAbsolute(entrance));
}

// Host side of part 2: take the exit a client asked for, if the host can change scenes right now.
void HostOnSceneRequest(uint32_t from, const nlohmann::json& message) {
    const int64_t entrance = Protocol::GetInt(message, "entrance", -1);
    const int64_t cutscene = Protocol::GetInt(message, "cutscene", 0xFFEF);
    const char* refusal = nullptr;
    if (entrance < 0 || entrance > 0xFFFF || !IsValidEntrance(static_cast<uint16_t>(entrance)) || cutscene < 0 ||
        cutscene > 0xFFFF) {
        refusal = "invalid entrance";
    } else if (!InGameplay() || gPlayState->transitionTrigger != TRANS_TRIGGER_OFF ||
               gPlayState->transitionMode != TRANS_MODE_OFF) {
        refusal = "the host is changing scenes";
    } else if (gPlayState->msgCtx.msgMode != MSGMODE_NONE || Play_InCsMode(gPlayState)) {
        refusal = "the host is in a text box or cutscene";
    } else if (GetLaunchOptions().testHostRefuseExits) {
        refusal = "--coop-test-host-refuse-exits";
    }
    if (refusal != nullptr) {
        SPDLOG_INFO("[Coop] Player {} asked to go to entrance 0x{:04X}; not now ({})", from,
                    static_cast<uint16_t>(entrance), refusal);
        return;
    }
    SPDLOG_INFO("[Coop] Player {} asked to go to entrance 0x{:04X}; the host takes the exit", from,
                static_cast<uint16_t>(entrance));
    gPlayState->nextEntrance = static_cast<uint16_t>(entrance);
    gSaveContext.nextCutsceneIndex = static_cast<uint16_t>(cutscene);
    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
    gPlayState->transitionType = TRANS_TYPE_FADE_BLACK;
    gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK;
}

// MARK: - Hooks

void OnSceneInit(s8 sceneId, s8 spawnNum) {
    if (IsHosting() && InGameplay()) {
        // The Song of Time (or any cycle reset) ends in a new scene with a higher reset count.
        const u16 resetCount = gSaveContext.save.saveInfo.playerData.threeDayResetCount;
        const bool cycleReset = sHostResetCount.has_value() && resetCount != *sHostResetCount;
        sHostResetCount = resetCount;
        HostSendSnapshot(cycleReset ? Snapshot::Reason::Cycle : Snapshot::Reason::Scene, kBroadcast);
        return;
    }
    sFollowInProgress = false;
    sHeldExit.active = false;
    if (sStage == ClientStage::Entering && InGameplay()) {
        sStage = ClientStage::InSession;
        SPDLOG_INFO("[Coop] Entered the session: scene {} spawn {} entrance 0x{:04X}", sceneId, spawnNum,
                    static_cast<uint16_t>(gSaveContext.save.entrance));
    }
    if (sStage == ClientStage::InSession) {
        PlayerSync::State host;
        const int hostRoom =
            PlayerSync::LatestState(kHostPlayerId, host) && host.sceneId == sceneId ? host.roomNum : -1;
        SPDLOG_INFO("[Coop] Scene check: local scene {} entrance 0x{:04X} room {}, host scene {} entrance 0x{:04X} "
                    "room {} ({})",
                    sceneId, static_cast<uint16_t>(gSaveContext.save.entrance), gPlayState->roomCtx.curRoom.num,
                    sLastHost.sceneId, static_cast<uint16_t>(sLastHost.entrance), hostRoom,
                    sceneId == sLastHost.sceneId ? "match" : "MISMATCH");
    }
}

// Rooms are reported but not followed: moving the client inside a scene needs the host's position (milestone 2).
void OnRoomInit(s8 sceneId, s8 roomNum) {
    if (IsHosting() && InGameplay()) {
        SendJson(kBroadcast, { { "type", "host_room" }, { "scene", sceneId }, { "room", roomNum } });
        SPDLOG_INFO("[Coop] Host entered room {} of scene {}", roomNum, sceneId);
    }
}

void ClientOnHostRoom(uint32_t, const nlohmann::json& message) {
    if (sStage == ClientStage::InSession) {
        SPDLOG_INFO("[Coop] Host is in room {} of scene {} (rooms are not followed in this milestone)",
                    Protocol::GetInt(message, "room", -1), Protocol::GetInt(message, "scene", -1));
    }
}

void OnPlayDestroy() {
    if (sStage == ClientStage::Entering) {
        // Play_Destroy of the title screen clears WEEKEVENTREG_92_80 after the snapshot was applied.
        gSaveContext.save.saveInfo.weekEventReg[92] = sEnterWeekEventReg92;
    }
}

// Decision (e): a client that loses the session sees why and goes back to the title screen without saving.
void ClientLeaveSession(const std::string& reason) {
    SPDLOG_WARN("[Coop] Session lost ({}). Returning to the title screen without saving.", reason);
    BenGui::RegisterPopup("Coop: disconnected", reason + "\n\nReturning to the title screen. Nothing was saved.");
    sStage = ClientStage::Leaving;
    sPending = {};
    sHeldExit = {};
    Clock::Reset();
    if (gGameState != nullptr) {
        STOP_GAMESTATE(gGameState);
        SET_NEXT_GAMESTATE(gGameState, ConsoleLogo_Init, sizeof(ConsoleLogoState));
    }
}

// The title screen starts from a fresh SaveContext, so the session state can no longer reach a save from here on.
void OnConsoleLogoUpdate() {
    if (sStage == ClientStage::Leaving) {
        sStage = ClientStage::None;
        SetClientInSession(false, "reached the title screen");
    }
}

void RegisterSessionHooks() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneInit>(OnSceneInit);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayDestroy>(OnPlayDestroy);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnRoomInit>(OnRoomInit);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnConsoleLogoUpdate>(OnConsoleLogoUpdate);
    RegisterJsonHandler("host_room", ClientOnHostRoom);
    RegisterJsonHandler("scene_request", HostOnSceneRequest);
    RegisterJsonHandler("enter_request", HostOnEnterRequest);
    RegisterJsonHandler("enter_denied", ClientOnEnterDenied);
    RegisterBinaryHandler(Protocol::kChannelSnapshot, ClientOnSnapshot);
}

} // namespace

void HostBroadcastSnapshot(uint16_t reason) {
    if (IsHosting() && InGameplay()) {
        HostSendSnapshot(static_cast<Snapshot::Reason>(reason), kBroadcast);
    }
}

void RequestEnterSession() {
    if (sStage != ClientStage::None || !IsConnectedClient()) {
        return;
    }
    sStage = ClientStage::Requested;
    sLastSnapshotSeq = 0;
    Clock::Reset();
    sPending = {};
    sFollowInProgress = false;
    sHeldExit = {};
    SendJson(kHostPlayerId, { { "type", "enter_request" } });
    SPDLOG_INFO("[Coop] Asked the host to enter the session");
}

bool IsClientInSession() {
    return sStage == ClientStage::Entering || sStage == ClientStage::InSession;
}

bool IsWaitingToEnter() {
    return sStage == ClientStage::Requested;
}

std::string SessionStatusText() {
    switch (sStage) {
        case ClientStage::Requested:
            return "Waiting for the host's snapshot...";
        case ClientStage::Entering:
            return "Entering the host's world...";
        case ClientStage::InSession:
            return fmt::format("In session on the host's world (scene {}). Your own saves are not touched.",
                               sLastHost.sceneId);
        default:
            return "";
    }
}

void SessionFrame() {
    if (sStage == ClientStage::Requested && !IsConnectedClient()) {
        sStage = ClientStage::None;
    }
    if ((sStage == ClientStage::Entering || sStage == ClientStage::InSession) && !IsConnectedClient()) {
        const SessionSnapshot net = GetSnapshot();
        ClientLeaveSession(net.message.empty() ? "The connection to the host was closed" : net.message);
        return;
    }
    if (sStage == ClientStage::InSession) {
        if (InGameplay()) {
            ClientBlockHostOnlyActions();
            ClientInterceptExit();
        }
        ClientUpdateSceneFollow();
    }

    // Test option --coop-test-enter: press "Enter session" once connected, retrying while the host is not in gameplay.
    static auto sLastTestEnter = std::chrono::steady_clock::time_point{};
    if (GetLaunchOptions().testEnter && sStage == ClientStage::None && IsConnectedClient() && gGameState != nullptr &&
        std::chrono::steady_clock::now() - sLastTestEnter > std::chrono::seconds(2)) {
        sLastTestEnter = std::chrono::steady_clock::now();
        RequestEnterSession();
    }
}

static RegisterShipInitFunc initFunc(RegisterSessionHooks);

} // namespace Coop
