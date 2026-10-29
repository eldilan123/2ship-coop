#include "CoopCycle.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include "Coop.h"
#include "CoopSession.h"
#include "CoopStory.h"
#include "2s2h/BenGui/Notification.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
}

namespace Coop::Cycle {

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr auto kRequestLifetime = std::chrono::seconds(60);

void Notify(const std::string& message) {
    Notification::Emit({ .message = message });
}

const char* KindKey(Kind kind) {
    switch (kind) {
        case Kind::Inverted:
            return "inverted";
        case Kind::Double:
            return "double";
        default:
            return "sot";
    }
}

const char* KindText(Kind kind) {
    switch (kind) {
        case Kind::Inverted:
            return "cambiar la velocidad del tiempo (Cancion del Tiempo invertida)";
        case Kind::Double:
            return "adelantar al siguiente amanecer o anochecer (Cancion del Tiempo doble)";
        default:
            return "volver al amanecer del primer dia (Cancion del Tiempo)";
    }
}

bool ParseKind(const std::string& key, Kind& out) {
    if (key == "sot") {
        out = Kind::SongOfTime;
    } else if (key == "inverted") {
        out = Kind::Inverted;
    } else if (key == "double") {
        out = Kind::Double;
    } else {
        return false;
    }
    return true;
}

std::string NameOf(uint32_t playerId) {
    for (const PlayerInfo& p : GetSnapshot().players) {
        if (p.id == playerId) {
            return p.name;
        }
    }
    return fmt::format("el jugador {}", playerId);
}

bool InGameplay() {
    return gPlayState != nullptr && gSaveContext.gameMode == GAMEMODE_NORMAL;
}

// MARK: - Host

struct Request {
    Kind kind;
    SteadyClock::time_point since;
};
std::unordered_map<uint32_t, Request> sRequests;
bool sSotWaitingForSave = false;

void Answer(uint32_t playerId, Kind kind, bool accepted, const char* why) {
    SendJson(playerId,
             { { "type", "cycle_answer" }, { "kind", KindKey(kind) }, { "accepted", accepted }, { "why", why } });
}

void HostOnRequest(uint32_t from, const nlohmann::json& m) {
    Kind kind;
    if (!IsHosting() || !m.contains("kind") || !m["kind"].is_string() ||
        !ParseKind(m["kind"].get<std::string>(), kind)) {
        return;
    }
    sRequests[from] = { kind, SteadyClock::now() };
    SPDLOG_INFO("[Coop] cycle-request: player {} asks to {} ({}); waiting for the host", from, KindKey(kind),
                KindText(kind));
    Notify(
        fmt::format("Coop: {} pide {}. Aceptalo o rechazalo en la ventana Coop (60 s)", NameOf(from), KindText(kind)));
}

// As Message_Update does when the player answers "Yes" to the Song of Time (MSGMODE_NEW_CYCLE_0): the end-of-cycle
// save, then (HostFrame) what Player does for OCARINA_MODE_APPLY_SOT once the save is written.
void HostRunSongOfTime() {
    PlayState* play = gPlayState;
    play->state.unk_A3 = 1;
    const s32 cutsceneIndex = gSaveContext.save.cutsceneIndex;
    const u16 time = CURRENT_TIME;
    const s32 day = gSaveContext.save.day;
    Sram_SaveEndOfCycle(play);
    gSaveContext.timerStates[TIMER_ID_MOON_CRASH] = TIMER_STATE_OFF;
    func_8014546C(&play->sramCtx);
    gSaveContext.save.day = day;
    gSaveContext.save.time = time;
    gSaveContext.save.cutsceneIndex = cutsceneIndex;
    if (gSaveContext.fileNum != 0xFF) {
        Sram_SetFlashPagesDefault(&play->sramCtx,
                                  gFlashSaveStartPages[gSaveContext.fileNum * FLASH_SAVE_MAIN_MULTIPLIER],
                                  gFlashSpecialSaveNumPages[gSaveContext.fileNum * FLASH_SAVE_MAIN_MULTIPLIER]);
        Sram_StartWriteToFlashDefault(&play->sramCtx);
    }
    sSotWaitingForSave = true;
}

void HostFinishSongOfTime() {
    PlayState* play = gPlayState;
    if (!sSotWaitingForSave || play == nullptr || (gSaveContext.fileNum != 0xFF && play->sramCtx.status != 0)) {
        return;
    }
    sSotWaitingForSave = false;
    play->nextEntrance =
        gSaveContext.save.saveInfo.playerData.threeDayResetCount == 1 ? ENTRANCE(CUTSCENE, 1) : ENTRANCE(CUTSCENE, 0);
    gSaveContext.nextCutsceneIndex = 0xFFF7;
    play->transitionTrigger = TRANS_TRIGGER_START;
    SPDLOG_INFO("[Coop] cycle: the host's end-of-cycle save is written; back to the dawn of the first day");
}

// The song's effect on the host's Link, as Player spawns it for the inverted and double songs.
void SpawnSongEffect(u16 ocarinaMode) {
    Player* link = GET_PLAYER(gPlayState);
    if (Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_TEST6, link->actor.world.pos.x, link->actor.world.pos.y,
                    link->actor.world.pos.z, 0, 0, 0, ocarinaMode) == nullptr) {
        SPDLOG_INFO("[Coop] cycle: the song's effect could not be spawned (the time change is done anyway)");
    }
}

void HostRun(Kind kind) {
    switch (kind) {
        case Kind::SongOfTime:
            HostRunSongOfTime();
            break;
        case Kind::Inverted:
            // As the inverted song's "Yes" does.
            if (gSaveContext.save.timeSpeedOffset == 0) {
                gSaveContext.save.timeSpeedOffset = -2;
                SpawnSongEffect(OCARINA_MODE_APPLY_INV_SOT_SLOW);
            } else {
                gSaveContext.save.timeSpeedOffset = 0;
                SpawnSongEffect(OCARINA_MODE_APPLY_INV_SOT_FAST);
            }
            break;
        case Kind::Double:
            // As the double song's "Yes" does; the clock channel sends the jump.
            gSaveContext.save.time = gSaveContext.save.isNight ? CLOCK_TIME(6, 0) : CLOCK_TIME(18, 0);
            gSaveContext.timerStates[TIMER_ID_MOON_CRASH] = TIMER_STATE_OFF;
            SpawnSongEffect(OCARINA_MODE_APPLY_DOUBLE_SOT);
            break;
    }
}

void HostFrame() {
    HostFinishSongOfTime();
    const auto now = SteadyClock::now();
    for (auto it = sRequests.begin(); it != sRequests.end();) {
        if (now - it->second.since > kRequestLifetime) {
            SPDLOG_INFO("[Coop] cycle-request: player {}'s request ({}) expired", it->first, KindKey(it->second.kind));
            Answer(it->first, it->second.kind, false, "expired");
            Notify(fmt::format("Coop: el pedido de {} caduco", NameOf(it->first)));
            it = sRequests.erase(it);
        } else {
            ++it;
        }
    }
}

// MARK: - Client

void ClientOnAnswer(uint32_t, const nlohmann::json& m) {
    Kind kind;
    if (IsHosting() || !m.contains("kind") || !m["kind"].is_string() ||
        !ParseKind(m["kind"].get<std::string>(), kind)) {
        return;
    }
    const bool accepted = m.value("accepted", false);
    const std::string why = m.value("why", std::string());
    SPDLOG_INFO("[Coop] cycle-answer: the host {} the request ({}{})", accepted ? "accepted" : "did not accept",
                KindKey(kind), why.empty() ? "" : ", " + why);
    if (accepted) {
        Notify("Coop: el host acepto; el tiempo cambia para los dos");
    } else if (why == "expired") {
        Notify("Coop: el host no respondio a tiempo (60 s)");
    } else {
        Notify("Coop: el host no acepto el cambio de tiempo");
    }
}

void RegisterCycleHooks() {
    RegisterJsonHandler("cycle_request", HostOnRequest);
    RegisterJsonHandler("cycle_answer", ClientOnAnswer);
}

} // namespace

void ClientRequest(Kind kind) {
    if (IsHosting()) {
        return;
    }
    SPDLOG_INFO("[Coop] cycle-request: asking the host to {}", KindKey(kind));
    SendJson(kHostPlayerId, { { "type", "cycle_request" }, { "kind", KindKey(kind) } });
    Notify(fmt::format("Coop: le pediste al host {}; tiene 60 s para aceptar", KindText(kind)));
}

void ClientEndOfCycle() {
    PlayState* play = gPlayState;
    if (play != nullptr && play->msgCtx.msgMode != MSGMODE_NONE) {
        Message_CloseTextbox(play);
    }
    for (int i = 0; i < TIMER_ID_MAX; i++) {
        gSaveContext.timerStates[i] = TIMER_STATE_OFF;
    }
    for (int i = 0; i < ARRAY_COUNT(gSaveContext.save.saveInfo.weekEventReg); i++) {
        gSaveContext.save.saveInfo.weekEventReg[i] &= ~Story::LocalWeekEventBits(i);
    }
    SPDLOG_INFO("[Coop] cycle: the client's own cycle ended (text box closed, timers off, its own weekEventReg bits "
                "cleared, nothing pending in the shared inventory)");
    Notify("Coop: el tiempo volvio al amanecer del primer dia");
}

std::vector<PendingRequest> HostPending() {
    std::vector<PendingRequest> out;
    const auto now = SteadyClock::now();
    for (const auto& [id, r] : sRequests) {
        const int left = static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(kRequestLifetime - (now - r.since)).count());
        out.push_back({ id, NameOf(id), KindText(r.kind), std::max(left, 0) });
    }
    return out;
}

const char* HostBusyReason() {
    PlayState* play = gPlayState;
    if (!InGameplay()) {
        return "not in gameplay";
    }
    if (sSotWaitingForSave || play->transitionTrigger != TRANS_TRIGGER_OFF || play->transitionMode != TRANS_MODE_OFF) {
        return "changing scenes";
    }
    if (play->msgCtx.msgMode != MSGMODE_NONE || IS_PAUSED(&play->pauseCtx)) {
        return "close the text box or the pause menu first";
    }
    if (CutsceneManager_GetCurrentCsId() != CS_ID_NONE || play->csCtx.state != CS_STATE_IDLE ||
        GET_PLAYER(play)->csAction != PLAYER_CSACTION_NONE || (GET_PLAYER(play)->stateFlags1 & PLAYER_STATE1_DEAD)) {
        return "wait for the cutscene to end";
    }
    return nullptr;
}

void HostAnswer(uint32_t playerId, bool accept) {
    auto it = sRequests.find(playerId);
    if (!IsHosting() || it == sRequests.end()) {
        return;
    }
    const Kind kind = it->second.kind;
    if (accept && HostBusyReason() != nullptr) {
        return;
    }
    sRequests.erase(it);
    SPDLOG_INFO("[Coop] cycle-request: the host {} player {}'s request ({})", accept ? "accepted" : "declined",
                playerId, KindKey(kind));
    Answer(playerId, kind, accept, accept ? "" : "declined");
    if (accept) {
        HostRun(kind);
    }
}

void Frame() {
    if (!IsHosting()) {
        sRequests.clear();
        sSotWaitingForSave = false;
        return;
    }
    HostFrame();
}

static RegisterShipInitFunc initFunc(RegisterCycleHooks);

} // namespace Coop::Cycle
