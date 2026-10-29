#include "CoopWorld.h"

#include <cmath>
#include <vector>
#include <spdlog/spdlog.h>
#include "Coop.h"
#include "CoopEnemies.h"
#include "CoopSession.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "overlays/actors/ovl_Door_Shutter/z_door_shutter.h"
#include "overlays/actors/ovl_En_Door/z_en_door.h"
}

namespace Coop::World {

namespace {

// Actors that only check their switch flag in Init: spawned again when it changes from the other game. The flag is
// (params >> shift) & 0x7F (each actor's own *_GET_SWITCH_FLAG macro).
struct InitOnly {
    s16 id;
    int shift;
};
constexpr InitOnly kInitOnly[] = {
    { ACTOR_BG_HAKUGIN_BOMBWALL, 0 }, { ACTOR_BG_IKANA_BOMBWALL, 0 }, { ACTOR_BG_ASTR_BOMBWALL, 0 },
    { ACTOR_BG_HAKA_BOMBWALL, 0 },    { ACTOR_BG_KIN2_BOMBWALL, 0 },  { ACTOR_BG_LAST_BWALL, 0 },
    { ACTOR_BG_SPDWEB, 8 },           { ACTOR_OBJ_LIGHTBLOCK, 8 },    { ACTOR_OBJ_BOMBIWA, 0 },
    { ACTOR_OBJ_HUGEBOMBIWA, 0 },     { ACTOR_OBJ_TARU, 0 },          { ACTOR_OBJ_PZLBLOCK, 0 },
    { ACTOR_OBJ_SYOKUDAI, 0 },        { ACTOR_OBJ_SWITCH, 8 },        { ACTOR_OBJ_LIGHTSWITCH, 8 },
};

// Objects with no flag of their own, shared by "broken here, break it there".
constexpr s16 kBreakables[] = { ACTOR_OBJ_TSUBO,   ACTOR_OBJ_FLOWERPOT, ACTOR_OBJ_KIBAKO,
                                ACTOR_OBJ_KIBAKO2, ACTOR_EN_ISHI,       ACTOR_EN_KUSA };

bool sQuiet = false; // Killing for the other game (respawn, remote break): not reported again
int sRespawns = 0;
int sUnlocks = 0;
int sBrokenSent = 0;
int sBrokenHere = 0;

bool InGameplay() {
    return gPlayState != nullptr && gSaveContext.gameMode == GAMEMODE_NORMAL;
}

bool RoomLoaded(s8 room) {
    const RoomContext& r = gPlayState->roomCtx;
    return room < 0 || room == r.curRoom.num || room == r.prevRoom.num;
}

bool Sharing() {
    return InGameplay() && (IsHosting() || IsClientPlayingInSession());
}

const InitOnly* FindInitOnly(s16 id) {
    for (const InitOnly& e : kInitOnly) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

bool IsBreakable(s16 id) {
    for (s16 b : kBreakables) {
        if (b == id) {
            return true;
        }
    }
    return false;
}

void Respawn(Actor* a, int flag) {
    const s16 id = a->id;
    const u16 params = static_cast<u16>(a->params);
    const Vec3f pos = a->home.pos;
    const Vec3s rot = a->home.rot;
    const s8 room = a->room;
    sQuiet = true;
    Actor_Kill(a);
    Actor* again = Enemies::SpawnLike(id, pos, rot, params);
    sQuiet = false;
    if (again != nullptr) {
        again->room = room;
    }
    sRespawns++;
    SPDLOG_INFO("[Coop] world: actor 0x{:03X} params 0x{:04X} at ({:.0f}, {:.0f}, {:.0f}) spawned again (switch "
                "flag 0x{:02X} changed in the other game){}",
                id, params, pos.x, pos.y, pos.z, flag, again != nullptr ? "" : "; it could not be spawned again");
}

void Unlock(Actor* a, int flag) {
    sUnlocks++;
    SPDLOG_INFO("[Coop] world: door 0x{:03X} params 0x{:04X} unlocked (switch flag 0x{:02X} set in the other game; no "
                "key spent here)",
                a->id, static_cast<u16>(a->params), flag);
}

// MARK: - Broken objects

bool ReadXYZ(const nlohmann::json& m, Vec3f& out) {
    if (!m.contains("pos") || !m["pos"].is_array() || m["pos"].size() != 3) {
        return false;
    }
    float v[3];
    for (int i = 0; i < 3; i++) {
        if (!m["pos"][i].is_number()) {
            return false;
        }
        v[i] = m["pos"][i].get<float>();
        if (!std::isfinite(v[i]) || std::fabs(v[i]) > 100000.0f) {
            return false;
        }
    }
    out = { v[0], v[1], v[2] };
    return true;
}

void Drop(const char* why) {
    SPDLOG_WARN("[Coop] Dropping world_obj: {}", why);
    NoteDroppedMessage();
}

void OnWorldObj(uint32_t from, const nlohmann::json& m) {
    if (!Sharing()) {
        return;
    }
    Vec3f pos;
    if (!m.contains("scene") || !m["scene"].is_number_integer() || !m.contains("id") || !m["id"].is_number_integer() ||
        !m.contains("params") || !m["params"].is_number_integer() || !ReadXYZ(m, pos)) {
        Drop("missing or bad scene, id, params or pos");
        return;
    }
    const int64_t id = m["id"].get<int64_t>();
    const int64_t params = m["params"].get<int64_t>();
    if (!IsBreakable(static_cast<s16>(id)) || id < 0 || id >= ACTOR_ID_MAX || params < 0 || params > 0xFFFF) {
        Drop("not an object that is shared this way");
        return;
    }
    if (m["scene"].get<int64_t>() != gPlayState->sceneId) {
        return; // Broken in a scene this game already left (or not reached yet)
    }
    if (IsHosting()) {
        // The host relays it to the other clients.
        for (const PlayerInfo& p : GetSnapshot().players) {
            if (!p.isHost && p.id != from) {
                SendJson(p.id, m);
            }
        }
    }
    for (int category = 0; category < ACTORCAT_MAX; category++) {
        for (Actor* a = gPlayState->actorCtx.actorLists[category].first; a != nullptr; a = a->next) {
            if (a->id == id && static_cast<u16>(a->params) == params && a->update != nullptr &&
                Math_Vec3f_DistXYZ(&a->home.pos, &pos) < 15.0f) {
                sQuiet = true;
                Actor_Kill(a);
                sQuiet = false;
                sBrokenHere++;
                SPDLOG_INFO("[Coop] world: object 0x{:03X} params 0x{:04X} at ({:.0f}, {:.0f}, {:.0f}) broken by "
                            "player {}; broken here too",
                            a->id, static_cast<u16>(params), pos.x, pos.y, pos.z, from);
                return;
            }
        }
    }
}

void OnActorKill(Actor* a) {
    if (sQuiet || !Sharing() || !IsBreakable(a->id) || !RoomLoaded(a->room) ||
        gPlayState->transitionTrigger != TRANS_TRIGGER_OFF) {
        return;
    }
    const nlohmann::json m = { { "type", "world_obj" },
                               { "scene", gPlayState->sceneId },
                               { "id", a->id },
                               { "params", static_cast<u16>(a->params) },
                               { "pos", { a->home.pos.x, a->home.pos.y, a->home.pos.z } } };
    SendJson(IsHosting() ? kBroadcast : kHostPlayerId, m);
    sBrokenSent++;
    SPDLOG_INFO("[Coop] world: object 0x{:03X} params 0x{:04X} at ({:.0f}, {:.0f}, {:.0f}) broken here; sent", a->id,
                static_cast<u16>(a->params), a->home.pos.x, a->home.pos.y, a->home.pos.z);
}

void LogStats() {
    static int64_t sNext = 0;
    const int64_t now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    if (now < sNext) {
        return;
    }
    sNext = now + 10000;
    SPDLOG_INFO("[Coop] world-stats: spawned again {}, doors unlocked {}, objects broken here and sent {}, broken "
                "from the other game {}",
                sRespawns, sUnlocks, sBrokenSent, sBrokenHere);
}

void OnFrame() {
    if (Sharing()) {
        LogStats();
    }
}

void RegisterWorldHooks() {
    RegisterJsonHandler("world_obj", OnWorldObj);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnActorKill>(OnActorKill);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameStateMainStart>(OnFrame);
}

} // namespace

void OnRemoteSwitchFlags(int word, uint32_t set, uint32_t cleared) {
    if (!InGameplay() || (set | cleared) == 0) {
        return;
    }
    const uint32_t changed = set | cleared;
    std::vector<std::pair<Actor*, int>> respawn;
    for (int category = 0; category < ACTORCAT_MAX; category++) {
        for (Actor* a = gPlayState->actorCtx.actorLists[category].first; a != nullptr; a = a->next) {
            if (a->update == nullptr || !RoomLoaded(a->room)) {
                continue;
            }
            int flag = -1;
            bool door = false;
            if (a->id == ACTOR_DOOR_SHUTTER) {
                // Small key and boss doors decide their lock in Init.
                const int type = DOORSHUTTER_GET_TYPE(a);
                if (type == 4 || type == 5) {
                    flag = DOORSHUTTER_GET_SWITCH_FLAG(a);
                    door = true;
                }
            } else if (a->id == ACTOR_EN_DOOR && ENDOOR_GET_TYPE(a) == ENDOOR_TYPE_LOCKED) {
                flag = reinterpret_cast<EnDoor*>(a)->typeVar.switchFlag;
                door = true;
            } else if (const InitOnly* e = FindInitOnly(a->id)) {
                flag = (a->params >> e->shift) & 0x7F;
                // A torch in a group re-reads its flag every frame; 0x7F means no flag.
                if (a->id == ACTOR_OBJ_SYOKUDAI && (flag == 0x7F || ((a->params >> 7) & 0xF) != 0)) {
                    flag = -1;
                }
            }
            if (flag < 0 || (flag >> 5) != word || !(changed & (1u << (flag & 0x1F)))) {
                continue;
            }
            if (door) {
                if (set & (1u << (flag & 0x1F))) {
                    if (a->id == ACTOR_DOOR_SHUTTER) {
                        reinterpret_cast<DoorShutter*>(a)->unk_166 = 0;
                    } else {
                        reinterpret_cast<EnDoor*>(a)->lockTimer = 0;
                    }
                    Unlock(a, flag);
                }
            } else {
                respawn.push_back({ a, flag });
            }
        }
    }
    for (const auto& [a, flag] : respawn) {
        Respawn(a, flag);
    }
}

void TestFuzz() {
    auto send = [](const char* what, const nlohmann::json& m) {
        SPDLOG_INFO("[Coop] TEST: fuzz world_obj: {}", what);
        SendJson(IsHosting() ? kBroadcast : kHostPlayerId, m);
    };
    if (!IsHosting() && !IsClientInSession()) {
        return;
    }
    send("no pos", { { "type", "world_obj" }, { "scene", 0 }, { "id", ACTOR_OBJ_TSUBO }, { "params", 0 } });
    send("pos of 2 numbers",
         { { "type", "world_obj" }, { "scene", 0 }, { "id", ACTOR_OBJ_TSUBO }, { "params", 0 }, { "pos", { 1, 2 } } });
    send("a door, not an object shared this way",
         { { "type", "world_obj" }, { "scene", 0 }, { "id", ACTOR_EN_DOOR }, { "params", 0 }, { "pos", { 0, 0, 0 } } });
    send("params 70000", { { "type", "world_obj" },
                           { "scene", 0 },
                           { "id", ACTOR_OBJ_TSUBO },
                           { "params", 70000 },
                           { "pos", { 0, 0, 0 } } });
    send("pos 1e9", { { "type", "world_obj" },
                      { "scene", 0 },
                      { "id", ACTOR_OBJ_TSUBO },
                      { "params", 0 },
                      { "pos", { 1e9, 0, 0 } } });
}

static RegisterShipInitFunc initFunc(RegisterWorldHooks);

} // namespace Coop::World
