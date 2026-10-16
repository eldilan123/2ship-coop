#include "CoopClock.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include "Coop.h"
#include "CoopArgs.h"
#include "CoopSession.h"
#include "CoopSnapshot.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "regs.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "overlays/actors/ovl_En_Test4/z_en_test4.h"
void EnTest4_GetBellTimeOnDay3(EnTest4* thisx);
void EnTest4_GetBellTimeAndShrinkScreenBeforeDay3(EnTest4* thisx, PlayState* play);
}

namespace Coop::Clock {

namespace {

using SteadyClock = std::chrono::steady_clock;

bool InGameplay() {
    return gPlayState != nullptr && gSaveContext.gameMode == GAMEMODE_NORMAL;
}

bool InCutscene() {
    return gPlayState != nullptr && (Play_InCsMode(gPlayState) || CutsceneManager_GetCurrentCsId() != CS_ID_NONE);
}

std::string FormatTime(uint16_t time) {
    const int minutes = time * 1440 / 0x10000;
    return fmt::format("{:02}:{:02}", minutes / 60, minutes % 60);
}

int16_t SignedDiff(uint16_t a, uint16_t b) {
    return static_cast<int16_t>(static_cast<uint16_t>(a - b));
}

// MARK: - Host

struct HostState {
    bool initialized = false;
    uint16_t prevTime = 0;
    int32_t prevDay = 0;
    SteadyClock::time_point lastSend;
    uint16_t timeAtLastSend = 0;
    uint32_t seq = 0;
} sHost;

void HostSend(uint16_t flags, float unitsPerSecond) {
    Message msg = {};
    msg.magic = kMagic;
    msg.seq = ++sHost.seq;
    msg.time = CURRENT_TIME;
    msg.flags = flags | (InCutscene() ? kFlagHostInCutscene : 0);
    msg.day = gSaveContext.save.day;
    msg.eventDayCount = gSaveContext.save.eventDayCount;
    msg.timeSpeedOffset = gSaveContext.save.timeSpeedOffset;
    msg.timeSpeed = R_TIME_SPEED;
    msg.unitsPerSecond = unitsPerSecond;
    SendBinary(kBroadcast, Protocol::kChannelClock, &msg, sizeof(msg));
}

// Runs at the start of the frame, so it sees the change made by the previous frame's update.
void HostFrame() {
    const uint16_t time = CURRENT_TIME;
    const int32_t day = gSaveContext.save.day;
    const SteadyClock::time_point now = SteadyClock::now();
    if (!sHost.initialized) {
        sHost = { true, time, day, now, time, sHost.seq };
        return;
    }

    const uint16_t step = static_cast<uint16_t>(time - sHost.prevTime);
    const bool jumped = day != sHost.prevDay || (step > kMaxStepPerFrame && step < 0x10000 - kMaxStepPerFrame);
    if (jumped) {
        // The snapshot first, so the client has the host's flags before it applies the new day/time.
        SPDLOG_INFO("[Coop] Host clock jump: {} day {} -> {} day {}", FormatTime(sHost.prevTime), sHost.prevDay,
                    FormatTime(time), day);
        HostBroadcastSnapshot(static_cast<uint16_t>(Snapshot::Reason::TimeJump));
        HostSend(kFlagJump, 0.0f);
        sHost.lastSend = now;
        sHost.timeAtLastSend = time;
    } else if (now - sHost.lastSend >= std::chrono::milliseconds(kSendIntervalMs)) {
        const float seconds = std::chrono::duration<float>(now - sHost.lastSend).count();
        const float rate = static_cast<uint16_t>(time - sHost.timeAtLastSend) / seconds;
        HostSend(0, rate);
        sHost.lastSend = now;
        sHost.timeAtLastSend = time;
    }
    sHost.prevTime = time;
    sHost.prevDay = day;
}

// MARK: - Client

struct ClientState {
    std::optional<Message> last;
    SteadyClock::time_point receivedAt;
    bool frozenLogged = false;
    bool hostInCutscene = false;
} sClient;

EnTest4* FindEnTest4() {
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_SWITCH].first; actor != nullptr; actor = actor->next) {
        if (actor->id == ACTOR_EN_TEST4) {
            return reinterpret_cast<EnTest4*>(actor);
        }
    }
    return nullptr;
}

// Like the Save Editor's UpdateGameTime, minus the parts that need the player.
void DoJump(uint16_t newTime, int32_t newDay, int32_t newEventDayCount, int32_t newTimeSpeedOffset) {
    const uint16_t prevTime = CURRENT_TIME;
    const int32_t prevDay = gSaveContext.save.day;
    const bool wasNight = (prevTime >= CLOCK_TIME(18, 0)) || (prevTime < CLOCK_TIME(6, 0));
    const bool isNight = (newTime >= CLOCK_TIME(18, 0)) || (newTime < CLOCK_TIME(6, 0));

    gSaveContext.save.time = newTime;
    gSaveContext.save.day = newDay;
    gSaveContext.save.eventDayCount = newEventDayCount;
    gSaveContext.save.timeSpeedOffset = newTimeSpeedOffset;
    gWeatherMode = WEATHER_MODE_CLEAR;
    gPlayState->envCtx.lightningState = LIGHTNING_OFF;

    if (isNight != wasNight) {
        if (gPlayState->sceneSequences.ambienceId != AMBIENCE_ID_13) {
            SEQCMD_STOP_SEQUENCE(SEQ_PLAYER_AMBIENCE, 0);
            SEQCMD_STOP_SEQUENCE(SEQ_PLAYER_BGM_MAIN, 240);
            gSaveContext.seqId = NA_BGM_DISABLED;
            gSaveContext.ambienceId = AMBIENCE_ID_DISABLED;
            Environment_PlaySceneSequence(gPlayState);
        }
        // Kills/spawns half-day actors.
        gPlayState->numSetupActors = -gPlayState->numSetupActors;
    }

    // Keep the day/night watcher from running its own transition over the new time.
    if (EnTest4* enTest4 = FindEnTest4()) {
        enTest4->prevTime = newTime;
        enTest4->prevBellTime = newTime;
        enTest4->daytimeIndex = isNight ? 0 : 1;
        if (CURRENT_DAY == 3) {
            EnTest4_GetBellTimeOnDay3(enTest4);
        } else {
            EnTest4_GetBellTimeAndShrinkScreenBeforeDay3(enTest4, gPlayState);
        }
        gSaveContext.screenScale = 1000.0f;
        gSaveContext.screenScaleFlag = false;
    }

    // The flags of the new day already came in the snapshot sent just before; these only refresh the clock texture
    // and the lighting for the day (re-copying the already synchronized scene flags is harmless).
    if (newDay != prevDay) {
        Interface_NewDay(gPlayState, CURRENT_DAY);
        Environment_NewDay(&gPlayState->envCtx);
    }
    SPDLOG_INFO("[Coop] Clock jump applied: {} day {} -> {} day {}", FormatTime(prevTime), prevDay, FormatTime(newTime),
                newDay);
}

void ClientOnClock(uint32_t, const std::vector<uint8_t>& bytes) {
    if (bytes.size() != sizeof(Message)) {
        SPDLOG_WARN("[Coop] Ignoring clock message of {} bytes (expected {})", bytes.size(), sizeof(Message));
        return;
    }
    Message msg;
    std::memcpy(&msg, bytes.data(), sizeof(msg));
    if (msg.magic != kMagic || !IsClientInSession() || (sClient.last.has_value() && msg.seq <= sClient.last->seq)) {
        return;
    }

    const bool hostInCutscene = (msg.flags & kFlagHostInCutscene) != 0;
    if (hostInCutscene && !sClient.hostInCutscene) {
        SPDLOG_INFO("[Coop] aviso: cutscene en el host (el cliente sigue por su cuenta)");
    }
    sClient.hostInCutscene = hostInCutscene;

    if ((msg.flags & kFlagJump) != 0 && InGameplay()) {
        DoJump(msg.time, msg.day, msg.eventDayCount, msg.timeSpeedOffset);
    } else if (sClient.last.has_value() && InGameplay()) {
        // Periodic correction: how far our extrapolation drifted from the host.
        const int16_t drift = SignedDiff(CURRENT_TIME, msg.time);
        if (std::abs(drift) > kSyncThreshold) {
            SPDLOG_WARN("[Coop] Clock drift {} units (local {} vs host {}), corrected", drift, FormatTime(CURRENT_TIME),
                        FormatTime(msg.time));
        }
    }
    if (sClient.frozenLogged) {
        SPDLOG_INFO("[Coop] Clock updates from the host resumed");
    }
    sClient.last = msg;
    sClient.receivedAt = SteadyClock::now();
    sClient.frozenLogged = false;
}

void ClientFrame() {
    if (!sClient.last.has_value() || !InGameplay()) {
        return;
    }
    const Message& msg = *sClient.last;
    auto elapsed = SteadyClock::now() - sClient.receivedAt;
    if (elapsed > std::chrono::milliseconds(kMaxExtrapolationMs)) {
        if (!sClient.frozenLogged) {
            sClient.frozenLogged = true;
            SPDLOG_WARN("[Coop] Clock: no update from the host for {:.1f} s, holding the time at {}",
                        kMaxExtrapolationMs / 1000.0, FormatTime(CURRENT_TIME));
        }
        elapsed = std::chrono::milliseconds(kMaxExtrapolationMs);
    }
    const float seconds = std::chrono::duration<float>(elapsed).count();
    gSaveContext.save.time = static_cast<uint16_t>(msg.time + static_cast<int>(msg.unitsPerSecond * seconds));
    gSaveContext.save.day = msg.day;
    gSaveContext.save.eventDayCount = msg.eventDayCount;
    gSaveContext.save.timeSpeedOffset = msg.timeSpeedOffset;
}

// MARK: - Shared

bool sLocalInCutscene = false;
int64_t sLastSampleSecond = 0;

void LogCutsceneNotice() {
    const bool inCutscene = InCutscene();
    if (inCutscene && !sLocalInCutscene && (IsHosting() || IsClientInSession())) {
        SPDLOG_INFO("[Coop] aviso: cutscene en {} (scene {}); no se sincroniza en este hito",
                    IsHosting() ? "el host" : "el cliente", gPlayState->sceneId);
    }
    sLocalInCutscene = inCutscene;
}

// --coop-test-clock-log: one sample per wall-clock second on both sides, so probar.ps1 can pair them.
void LogClockSample() {
    if (!GetLaunchOptions().testClockLog) {
        return;
    }
    const int64_t second =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (second == sLastSampleSecond) {
        return;
    }
    sLastSampleSecond = second;
    SPDLOG_INFO("[Coop] Clock sample: role={} scene={} time=0x{:04X} ({}) day={}", IsHosting() ? "host" : "client",
                gPlayState->sceneId, CURRENT_TIME, FormatTime(CURRENT_TIME), gSaveContext.save.day);
}

void RegisterClockHooks() {
    // While a client is in a session its clock only moves with the host's messages.
    REGISTER_VB_SHOULD(VB_ADVANCE_TIME, {
        if (IsClientInSession()) {
            *should = false;
        }
    });
    RegisterBinaryHandler(Protocol::kChannelClock, ClientOnClock);
}

} // namespace

void Frame() {
    if (!InGameplay()) {
        return;
    }
    if (IsHosting()) {
        HostFrame();
    } else if (IsClientInSession()) {
        ClientFrame();
    } else {
        return;
    }
    LogCutsceneNotice();
    LogClockSample();
}

void JumpTo(uint16_t time, int32_t day) {
    if (InGameplay()) {
        DoJump(time, day, gSaveContext.save.eventDayCount, gSaveContext.save.timeSpeedOffset);
    }
}

void Reset() {
    sClient = {};
}

static RegisterShipInitFunc initFunc(RegisterClockHooks);

} // namespace Coop::Clock
