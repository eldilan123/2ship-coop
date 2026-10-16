#include "CoopSession.h"

#include <chrono>
#include <cstring>
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include "Coop.h"
#include "CoopArgs.h"
#include "CoopSaveGuard.h"
#include "CoopSnapshot.h"
#include "2s2h/BenGui/Notification.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "variables.h"
#include "functions.h"
#include "macros.h"
}

namespace Coop {

namespace {

// MARK: - Client state

enum class ClientStage {
    None,      // Not in a session
    Requested, // "Enter session" sent, waiting for the join snapshot
    Entering,  // Snapshot applied, switching to Play
    InSession, // Playing on the host's world
};

ClientStage sStage = ClientStage::None;
uint32_t sLastSnapshotSeq = 0;
Snapshot::Header sLastHost = {}; // Header of the last applied snapshot
uint8_t sEnterWeekEventReg92 = 0;

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

void OnPlayDestroy() {
    if (sStage == ClientStage::Entering) {
        // Play_Destroy of the title screen clears WEEKEVENTREG_92_80 after the snapshot was applied.
        gSaveContext.save.saveInfo.weekEventReg[92] = sEnterWeekEventReg92;
    }
}

void RegisterSessionHooks() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneInit>(OnSceneInit);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayDestroy>(OnPlayDestroy);
    RegisterJsonHandler("enter_request", HostOnEnterRequest);
    RegisterJsonHandler("enter_denied", ClientOnEnterDenied);
    RegisterBinaryHandler(Protocol::kChannelSnapshot, ClientOnSnapshot);
}

} // namespace

void RequestEnterSession() {
    if (sStage != ClientStage::None || !IsConnectedClient()) {
        return;
    }
    sStage = ClientStage::Requested;
    sLastSnapshotSeq = 0;
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
