#include "CoopSession.h"

#include <chrono>
#include <cstring>
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include "Coop.h"
#include "CoopArgs.h"
#include "CoopClock.h"
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
};
PendingDestination sPending = {};

// MARK: - Host state

uint32_t sNextSnapshotSeq = 1;

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
    sPending = { true, h.entrance, h.sceneLayer, h.sceneId, h.seq };
}

// Same as the debug console "entrance" command: let Play's own transition code load the host's scene.
void ClientFollowHost(const PendingDestination& dest) {
    const uint16_t entrance = static_cast<uint16_t>(dest.entrance);
    gPlayState->nextEntrance = entrance & ~0xF;
    // A scene layer is selected through the cutscene index, as cutscene destinations do (see z_demo.c).
    gSaveContext.nextCutsceneIndex = dest.sceneLayer > 0 ? 0xFFEF + dest.sceneLayer : 0xFFEF;
    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
    gPlayState->transitionType = TRANS_TYPE_FADE_BLACK;
    gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK;
    SPDLOG_INFO("[Coop] Following the host from scene {} to scene {} (entrance 0x{:04X}, layer {})",
                gPlayState->sceneId, dest.sceneId, entrance, dest.sceneLayer);
}

void ClientUpdateSceneFollow() {
    if (!sPending.active || !InGameplay()) {
        return;
    }
    // Wait for any transition in progress (ours or the arrival fade) to finish.
    if (gPlayState->transitionTrigger != TRANS_TRIGGER_OFF || gPlayState->transitionMode != TRANS_MODE_OFF) {
        return;
    }
    const PendingDestination dest = sPending;
    sPending.active = false;
    if (gPlayState->sceneId == dest.sceneId) {
        return;
    }
    ClientFollowHost(dest);
}

// MARK: - Hooks

void OnSceneInit(s8 sceneId, s8 spawnNum) {
    if (IsHosting() && InGameplay()) {
        HostSendSnapshot(Snapshot::Reason::Scene, kBroadcast);
        return;
    }
    if (sStage == ClientStage::Entering && InGameplay()) {
        sStage = ClientStage::InSession;
        SPDLOG_INFO("[Coop] Entered the session: scene {} spawn {} entrance 0x{:04X}", sceneId, spawnNum,
                    static_cast<uint16_t>(gSaveContext.save.entrance));
    }
    if (sStage == ClientStage::InSession) {
        SPDLOG_INFO("[Coop] Scene check: local scene {} entrance 0x{:04X}, host scene {} entrance 0x{:04X} ({})",
                    sceneId, static_cast<uint16_t>(gSaveContext.save.entrance), sLastHost.sceneId,
                    static_cast<uint16_t>(sLastHost.entrance), sceneId == sLastHost.sceneId ? "match" : "MISMATCH");
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
