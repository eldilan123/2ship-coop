#include "CoopCutscene.h"

#include <chrono>
#include <cmath>
#include <string>
#include <unordered_map>
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include "Coop.h"
#include "CoopPlayerSync.h"
#include "CoopSession.h"
#include "2s2h/BenGui/Notification.h"
#include "2s2h/Enhancements/Trackers/TimeSplits/Timesplits.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "overlays/actors/ovl_En_Box/z_en_box.h"
}

namespace Coop::Cutscene {

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr auto kMirrorStartMs = std::chrono::milliseconds(3000); // The host's cutscene must start by then
constexpr auto kMirrorMaxMs = std::chrono::milliseconds(60000);
constexpr float kMirrorActorRange = 50.0f; // The host's copy of the actor: same id and params, home this close

void Notify(const std::string& message) {
    Notification::Emit({ .message = message });
}

bool InGameplay() {
    return gPlayState != nullptr && gSaveContext.gameMode == GAMEMODE_NORMAL;
}

bool InSession() {
    return (IsHosting() && GetSnapshot().players.size() > 1) || IsClientPlayingInSession();
}

std::string NameOf(uint32_t playerId) {
    for (const PlayerInfo& info : GetSnapshot().players) {
        if (info.id == playerId) {
            return info.id == kHostPlayerId ? std::string("el host") : info.name;
        }
    }
    return fmt::format("el jugador {}", playerId);
}

// MARK: - "<player> consiguio <item>"

// Items worth a notice: not rupees, hearts, refills and ammo, magic jars, tokens or stray fairies.
bool Matters(u8 item) {
    if (item == ITEM_DEKU_STICK_UPGRADE_20 || item == ITEM_DEKU_STICK_UPGRADE_30 || item == ITEM_DEKU_NUT_UPGRADE_30 ||
        item == ITEM_DEKU_NUT_UPGRADE_40) {
        return true;
    }
    if (item >= ITEM_RECOVERY_HEART || (item >= ITEM_INVALID_1 && item <= ITEM_INVALID_7)) {
        return false;
    }
    switch (item) {
        case ITEM_BOMB:
        case ITEM_BOMBCHU:
        case ITEM_DEKU_STICK:
        case ITEM_DEKU_NUT:
        case ITEM_MAGIC_BEANS:
        case ITEM_SKULL_TOKEN:
        case ITEM_STRAY_FAIRIES:
        case ITEM_MAGIC_JAR_SMALL:
        case ITEM_MAGIC_JAR_BIG:
        case ITEM_71:
        case ITEM_72:
            return false;
        default:
            return true;
    }
}

std::string ItemName(u8 item) {
    for (const TimesplitObject& o : splitObjectList) {
        if (o.splitId == item) {
            return o.splitName;
        }
    }
    return fmt::format("el objeto 0x{:02X}", item);
}

void OnItemGive(u8 item) {
    if (!InGameplay() || !InSession() || !Matters(item)) {
        return;
    }
    const nlohmann::json message = { { "type", "got_item" }, { "item", item } };
    SendJson(IsHosting() ? kBroadcast : kHostPlayerId, message);
}

void OnGotItem(uint32_t from, const nlohmann::json& message) {
    const int item = message.value("item", -1);
    if (item < 0 || item > 0xFF || !Matters(static_cast<u8>(item))) {
        return;
    }
    // The host names the client and passes it on; a client hears it from the host (with who got it).
    uint32_t who = from;
    if (IsHosting()) {
        for (const PlayerInfo& info : GetSnapshot().players) {
            if (info.id != from && info.id != kHostPlayerId) {
                SendJson(info.id, { { "type", "got_item" }, { "item", item }, { "who", from } });
            }
        }
    } else {
        who = message.value("who", kHostPlayerId);
    }
    SPDLOG_INFO("[Coop] got-item: player {} got item 0x{:02X}", who, item);
    Notify(fmt::format("Coop: {} consiguio {}", NameOf(who), ItemName(static_cast<u8>(item))));
}

// MARK: - "<player> esta en una cinematica"

struct RemoteCutscene {
    bool in = false;
    bool noticed = false;
    SteadyClock::time_point since;
};
std::unordered_map<uint32_t, RemoteCutscene> sRemoteInCutscene;
// Shorter ones (a door, the entrance on joining) get no notice.
constexpr auto kNoticeAfter = std::chrono::milliseconds(1000);

void NoticeRemoteCutscenes() {
    const auto now = SteadyClock::now();
    const SessionSnapshot snapshot = GetSnapshot();
    for (const PlayerInfo& info : snapshot.players) {
        if (info.id == snapshot.localId) {
            continue;
        }
        PlayerSync::State s;
        uint32_t age = 0;
        const bool inCutscene =
            PlayerSync::LatestState(info.id, s, &age) && age < 1000 && (s.busy & PlayerSync::kBusyCutscene) != 0;
        RemoteCutscene& r = sRemoteInCutscene[info.id];
        if (inCutscene != r.in) {
            r.in = inCutscene;
            r.noticed = false;
            r.since = now;
            SPDLOG_INFO("[Coop] cutscene-notice: player {} {} a cutscene", info.id, inCutscene ? "entered" : "left");
        } else if (r.in && !r.noticed && now - r.since >= kNoticeAfter) {
            r.noticed = true;
            Notify(fmt::format("Coop: {} esta en una cinematica", NameOf(info.id)));
        }
    }
}

// MARK: - The mirror (level 2)

bool Allowlisted(const Actor* actor) {
    switch (actor->id) {
        case ACTOR_EN_OWL:
        case ACTOR_BG_DY_YOSEIZO:
        case ACTOR_EN_ELFGRP:
        case ACTOR_EN_OSN:
        case ACTOR_EN_HG:
        case ACTOR_EN_GG:
        case ACTOR_EN_ZOG:
            return true;
        case ACTOR_EN_BOX: {
            const int type = ENBOX_GET_TYPE(actor);
            return type != ENBOX_TYPE_SMALL && type != ENBOX_TYPE_SMALL_INVISIBLE &&
                   type != ENBOX_TYPE_SMALL_ROOM_CLEAR && type != ENBOX_TYPE_SMALL_SWITCH_FLAG_FALL &&
                   type != ENBOX_TYPE_SMALL_SWITCH_FLAG;
        }
        default:
            return false;
    }
}

// Client: the allowlisted cutscene it is in (sent to the host), so its end can be sent too.
s16 sClientMirrored = CS_ID_NONE;

void ClientOnStartCutscene(s16 csId, Actor* actor) {
    if (actor == nullptr || !Allowlisted(actor) || sClientMirrored != CS_ID_NONE) {
        return;
    }
    sClientMirrored = csId;
    SendJson(kHostPlayerId, { { "type", "mirror_cs" },
                              { "scene", gPlayState->sceneId },
                              { "layer", gSaveContext.sceneLayer },
                              { "cs", csId },
                              { "actor", actor->id },
                              { "params", actor->params },
                              { "x", actor->home.pos.x },
                              { "y", actor->home.pos.y },
                              { "z", actor->home.pos.z } });
}

void ClientFrame() {
    if (sClientMirrored != CS_ID_NONE && CutsceneManager_GetCurrentCsId() != sClientMirrored) {
        SendJson(kHostPlayerId, { { "type", "mirror_cs_end" }, { "cs", sClientMirrored } });
        sClientMirrored = CS_ID_NONE;
    }
}

struct Mirror {
    enum class Phase { None, Queued, Running } phase = Phase::None;
    s16 csId = CS_ID_NONE;
    Actor* actor = nullptr;
    uint32_t from = 0;
    SteadyClock::time_point since;
    bool clientEnded = false;
};
Mirror sMirror;
uint32_t sMirrorStarted = 0, sMirrorSkipped = 0;

void Skip(uint32_t from, s16 csId, const char* why) {
    sMirrorSkipped++;
    SPDLOG_INFO("[Coop] cutscene-mirror: player {} cutscene {} not mirrored ({})", from, csId, why);
}

const char* HostBusy() {
    PlayState* play = gPlayState;
    Player* link = GET_PLAYER(play);
    if (CutsceneManager_GetCurrentCsId() != CS_ID_NONE || play->csCtx.state != CS_STATE_IDLE ||
        link->csAction != PLAYER_CSACTION_NONE) {
        return "the host is in a cutscene";
    }
    if (play->msgCtx.msgMode != MSGMODE_NONE || IS_PAUSED(&play->pauseCtx)) {
        return "the host is in a text box or the pause menu";
    }
    if (play->transitionTrigger != TRANS_TRIGGER_OFF || play->transitionMode != TRANS_MODE_OFF ||
        (link->stateFlags1 & PLAYER_STATE1_DEAD)) {
        return "the host is changing scenes or dead";
    }
    return nullptr;
}

void HostOnMirror(uint32_t from, const nlohmann::json& m) {
    const s16 csId = static_cast<s16>(m.value("cs", -1));
    if (!InGameplay()) {
        return;
    }
    if (CVarGetInteger(CVAR_COOP_MIRROR_CUTSCENES, 0) == 0) {
        Skip(from, csId, "gCoop.MirrorCutscenes = 0");
        return;
    }
    if (sMirror.phase != Mirror::Phase::None) {
        Skip(from, csId, "another one is being mirrored");
        return;
    }
    if (m.value("scene", -1) != gPlayState->sceneId || m.value("layer", -1) != gSaveContext.sceneLayer) {
        Skip(from, csId, "the host is in another scene");
        return;
    }
    if (const char* why = HostBusy()) {
        Skip(from, csId, why);
        return;
    }
    const s16 id = static_cast<s16>(m.value("actor", -1));
    const s16 params = static_cast<s16>(m.value("params", 0));
    Vec3f home = { m.value("x", 0.0f), m.value("y", 0.0f), m.value("z", 0.0f) };
    Actor* found = nullptr;
    for (int category = 0; category < ACTORCAT_MAX && found == nullptr; category++) {
        for (Actor* a = gPlayState->actorCtx.actorLists[category].first; a != nullptr; a = a->next) {
            if (a->id == id && a->params == params && a->update != nullptr && Allowlisted(a) &&
                Math_Vec3f_DistXYZ(&a->home.pos, &home) <= kMirrorActorRange) {
                found = a;
                break;
            }
        }
    }
    if (found == nullptr) {
        Skip(from, csId, "no copy of the actor here");
        return;
    }
    sMirror = {};
    sMirror.phase = Mirror::Phase::Queued;
    sMirror.csId = csId;
    sMirror.actor = found;
    sMirror.from = from;
    sMirror.since = SteadyClock::now();
    CutsceneManager_Queue(csId);
    SPDLOG_INFO("[Coop] cutscene-mirror: player {} cutscene {} (actor 0x{:03X}); queued on the host", from, csId, id);
}

void HostOnMirrorEnd(uint32_t from, const nlohmann::json& m) {
    if (sMirror.phase != Mirror::Phase::None && sMirror.from == from && m.value("cs", -1) == sMirror.csId) {
        sMirror.clientEnded = true;
    }
}

void EndMirror(const char* why) {
    PlayState* play = gPlayState;
    if (sMirror.phase == Mirror::Phase::Running && CutsceneManager_GetCurrentCsId() == sMirror.csId) {
        // A scripted one is stopped as its script would; the manager then ends it (or at the end of its length).
        if (play->csCtx.state != CS_STATE_IDLE) {
            play->csCtx.state = CS_STATE_STOP;
        }
        CutsceneManager_Stop(CS_ID_GLOBAL_END);
    }
    SPDLOG_INFO("[Coop] cutscene-mirror: cutscene {} of player {} over on the host ({}); started {}, skipped {}",
                sMirror.csId, sMirror.from, why, sMirrorStarted, sMirrorSkipped);
    sMirror = {};
}

void HostFrame() {
    if (sMirror.phase == Mirror::Phase::None) {
        return;
    }
    const auto now = SteadyClock::now();
    if (sMirror.phase == Mirror::Phase::Queued) {
        if (sMirror.clientEnded || now - sMirror.since > kMirrorStartMs) {
            EndMirror(sMirror.clientEnded ? "the client's ended before it started" : "it did not start in time");
        } else if (CutsceneManager_IsNext(sMirror.csId) > 0) {
            CutsceneManager_StartWithPlayerCs(sMirror.csId, sMirror.actor);
            sMirror.phase = Mirror::Phase::Running;
            sMirror.since = now;
            sMirrorStarted++;
            SPDLOG_INFO("[Coop] cutscene-mirror: cutscene {} started on the host", sMirror.csId);
        } else {
            CutsceneManager_Queue(sMirror.csId);
        }
        return;
    }
    if (CutsceneManager_GetCurrentCsId() != sMirror.csId) {
        EndMirror("it ended by itself");
    } else if (sMirror.clientEnded) {
        EndMirror("the client's ended");
    } else if (now - sMirror.since > kMirrorMaxMs) {
        EndMirror("time limit");
    }
}

void OnActorDestroy(Actor* actor) {
    if (actor == sMirror.actor) {
        sMirror.actor = nullptr;
        if (sMirror.phase == Mirror::Phase::Queued) {
            sMirror = {};
        }
    }
}

void RegisterCutsceneHooks() {
    auto* gi = GameInteractor::Instance;
    gi->RegisterGameHook<GameInteractor::OnItemGive>(OnItemGive);
    gi->RegisterGameHook<GameInteractor::OnActorDestroy>(OnActorDestroy);
    REGISTER_VB_SHOULD(VB_START_CUTSCENE, {
        if (!*should || IsHosting() || !IsClientPlayingInSession() || !InGameplay()) {
            return;
        }
        const s16 csId = *va_arg(args, s16*);
        Actor* actor = va_arg(args, Actor*);
        if (csId > CS_ID_NONE && CutsceneManager_GetCurrentCsId() == CS_ID_NONE) {
            ClientOnStartCutscene(csId, actor);
        }
    });
    RegisterJsonHandler("got_item", OnGotItem);
    RegisterJsonHandler("mirror_cs", [](uint32_t from, const nlohmann::json& m) {
        if (IsHosting()) {
            HostOnMirror(from, m);
        }
    });
    RegisterJsonHandler("mirror_cs_end", [](uint32_t from, const nlohmann::json& m) {
        if (IsHosting()) {
            HostOnMirrorEnd(from, m);
        }
    });
}

} // namespace

void Frame() {
    if (!InGameplay() || (!IsHosting() && !IsClientInSession())) {
        sRemoteInCutscene.clear();
        sClientMirrored = CS_ID_NONE;
        sMirror = {};
        return;
    }
    if (IsHosting()) {
        HostFrame();
    } else {
        ClientFrame();
    }
    NoticeRemoteCutscenes();
}

static RegisterShipInitFunc initFunc(RegisterCutsceneHooks);

} // namespace Coop::Cutscene
