#include "CoopShared.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include <spdlog/spdlog.h>
#include "Coop.h"
#include "CoopRooms.h"
#include "CoopSaveGuard.h"
#include "CoopSession.h"
#include "CoopStory.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
}

namespace Coop::Shared {

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr int64_t kCheckEveryMs = 2000;
constexpr int64_t kStatsEveryMs = 10000;

enum class Group { Inventory, Flags, World };

const char* GroupCounter(Group g) {
    switch (g) {
        case Group::Inventory:
            return "inv-desync";
        case Group::Flags:
            return "flag-desync";
        case Group::World:
            return "world-desync";
    }
    return "?";
}

// One shared field: its name in the logs, its group and how to read its bytes.
struct Field {
    const char* name;
    Group group;
    std::function<void(std::vector<uint8_t>&)> read;
};

void Put(std::vector<uint8_t>& out, const void* data, size_t size) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

#define RAW(expr) [](std::vector<uint8_t>& out) { Put(out, &(expr), sizeof(expr)); }

// The shared part of the save, field by field. Each player's own values are not here: health, magic, form, worn mask,
// equipped buttons (equips.buttonItems / cButtonSlots, D-pad), Tatl's timer, the clock, the entrance and respawn data.
const std::vector<Field>& Fields() {
    static const std::vector<Field> sFields = [] {
        std::vector<Field> f;
        // Inventory and economy
        f.push_back({ "healthCapacity", Group::Inventory, RAW(gSaveContext.save.saveInfo.playerData.healthCapacity) });
        f.push_back(
            { "magic acquired / double magic / double defense", Group::Inventory, [](std::vector<uint8_t>& out) {
                 const SavePlayerData& pd = gSaveContext.save.saveInfo.playerData;
                 const u8 v[3] = { pd.isMagicAcquired, pd.isDoubleMagicAcquired, pd.doubleDefense };
                 Put(out, v, sizeof(v));
             } });
        // Rupees still in the accumulator count: Interface_Update moves them a few per frame.
        f.push_back({ "rupees", Group::Inventory, [](std::vector<uint8_t>& out) {
                         const s32 v = std::clamp<s32>(gSaveContext.save.saveInfo.playerData.rupees +
                                                           gSaveContext.rupeeAccumulator,
                                                       0, CUR_CAPACITY(UPG_WALLET));
                         Put(out, &v, sizeof(v));
                     } });
        f.push_back({ "swordHealth", Group::Inventory, RAW(gSaveContext.save.saveInfo.playerData.swordHealth) });
        f.push_back({ "equipment", Group::Inventory, RAW(gSaveContext.save.saveInfo.equips.equipment) });
        f.push_back({ "items", Group::Inventory, RAW(gSaveContext.save.saveInfo.inventory.items) });
        f.push_back({ "ammo", Group::Inventory, RAW(gSaveContext.save.saveInfo.inventory.ammo) });
        f.push_back({ "upgrades", Group::Inventory, RAW(gSaveContext.save.saveInfo.inventory.upgrades) });
        f.push_back({ "questItems", Group::Inventory, RAW(gSaveContext.save.saveInfo.inventory.questItems) });
        f.push_back({ "dungeonItems", Group::Inventory, RAW(gSaveContext.save.saveInfo.inventory.dungeonItems) });
        f.push_back({ "dungeonKeys", Group::Inventory, RAW(gSaveContext.save.saveInfo.inventory.dungeonKeys) });
        f.push_back({ "defenseHearts", Group::Inventory, RAW(gSaveContext.save.saveInfo.inventory.defenseHearts) });
        f.push_back({ "strayFairies", Group::Inventory, RAW(gSaveContext.save.saveInfo.inventory.strayFairies) });
        f.push_back({ "skullTokenCount", Group::Inventory, RAW(gSaveContext.save.saveInfo.skullTokenCount) });
        f.push_back({ "bank rupees", Group::Inventory, [](std::vector<uint8_t>& out) {
                         const u32 v = HS_GET_BANK_RUPEES();
                         Put(out, &v, sizeof(v));
                     } });
        f.push_back({ "stolenItems", Group::Inventory, RAW(gSaveContext.save.saveInfo.stolenItems) });
        // Story: weekEventReg in blocks of 10 bytes, so a log line says roughly where
        for (int block = 0; block < 10; block++) {
            static const char* kNames[] = { "weekEventReg[0-9]",   "weekEventReg[10-19]", "weekEventReg[20-29]",
                                            "weekEventReg[30-39]", "weekEventReg[40-49]", "weekEventReg[50-59]",
                                            "weekEventReg[60-69]", "weekEventReg[70-79]", "weekEventReg[80-89]",
                                            "weekEventReg[90-99]" };
            // Each player's own bits (CoopStory.cpp) are left out.
            f.push_back({ kNames[block], Group::Flags, [block](std::vector<uint8_t>& out) {
                             for (int k = block * 10; k < block * 10 + 10; k++) {
                                 const u8 v =
                                     gSaveContext.save.saveInfo.weekEventReg[k] & ~Story::LocalWeekEventBits(k);
                                 Put(out, &v, 1);
                             }
                         } });
        }
        // eventInf is each player's own (temporary state: minigames, the day's telop...): not compared.
        f.push_back({ "permanentSceneFlags", Group::Flags, RAW(gSaveContext.save.saveInfo.permanentSceneFlags) });
        // The current scene's entry is left out: both sides only copy their live flags there when they leave it (the
        // live flags are the world group).
        f.push_back({ "cycleSceneFlags (other scenes)", Group::Flags, [](std::vector<uint8_t>& out) {
                         const int current = gPlayState != nullptr ? Play_GetOriginalSceneId(gPlayState->sceneId) : -1;
                         for (int i = 0; i < ARRAY_COUNT(gSaveContext.cycleSceneFlags); i++) {
                             if (i != current) {
                                 Put(out, &gSaveContext.cycleSceneFlags[i], sizeof(gSaveContext.cycleSceneFlags[i]));
                             }
                         }
                     } });
        f.push_back({ "owl statues", Group::Flags, RAW(gSaveContext.save.saveInfo.playerData.owlActivationFlags) });
        f.push_back({ "maps and regions", Group::Flags, [](std::vector<uint8_t>& out) {
                         const SaveInfo& s = gSaveContext.save.saveInfo;
                         Put(out, s.scenesVisible, sizeof(s.scenesVisible));
                         Put(out, &s.regionsVisited, sizeof(s.regionsVisited));
                         Put(out, &s.worldMapCloudVisibility, sizeof(s.worldMapCloudVisibility));
                     } });
        f.push_back({ "masksGivenOnMoon", Group::Flags, RAW(gSaveContext.masksGivenOnMoon) });
        f.push_back({ "story misc (Tatl, Snowhead, first cycle, gossip stones, blue warps)", Group::Flags,
                      [](std::vector<uint8_t>& out) {
                          const Save& s = gSaveContext.save;
                          const u8 v[3] = { s.hasTatl, s.snowheadCleared, s.isFirstCycle };
                          Put(out, v, sizeof(v));
                          Put(out, &s.saveInfo.unk_EA0, sizeof(s.saveInfo.unk_EA0));
                          Put(out, s.saveInfo.unk_EA8, sizeof(s.saveInfo.unk_EA8));
                      } });
        f.push_back({ "bombers, lottery and spider house codes", Group::Flags, [](std::vector<uint8_t>& out) {
                         const SaveInfo& s = gSaveContext.save.saveInfo;
                         Put(out, &s.bombersCaughtNum, sizeof(s.bombersCaughtNum));
                         Put(out, s.bombersCaughtOrder, sizeof(s.bombersCaughtOrder));
                         Put(out, s.lotteryCodes, sizeof(s.lotteryCodes));
                         Put(out, s.spiderHouseMaskOrder, sizeof(s.spiderHouseMaskOrder));
                         Put(out, s.bomberCode, sizeof(s.bomberCode));
                     } });
        f.push_back({ "scarecrow song", Group::Flags, [](std::vector<uint8_t>& out) {
                         const SaveInfo& s = gSaveContext.save.saveInfo;
                         Put(out, &s.unk_F40, sizeof(s.unk_F40));
                         Put(out, &s.scarecrowSpawnSongSet, sizeof(s.scarecrowSpawnSongSet));
                         Put(out, s.scarecrowSpawnSong, sizeof(s.scarecrowSpawnSong));
                     } });
        f.push_back({ "records (high scores without the bank, pictograph, aliens)", Group::Flags,
                      [](std::vector<uint8_t>& out) {
                          const SaveInfo& s = gSaveContext.save.saveInfo;
                          Put(out, &s.highScores[HS_BANK_RUPEES + 1],
                              sizeof(s.highScores) - sizeof(s.highScores[HS_BANK_RUPEES]));
                          Put(out, s.dekuPlaygroundHighScores, sizeof(s.dekuPlaygroundHighScores));
                          Put(out, &s.pictoFlags0, sizeof(s.pictoFlags0));
                          Put(out, s.alienInfo, sizeof(s.alienInfo));
                      } });
        // The live flags of the current scene (ActorContextSceneFlags)
        f.push_back({ "live switch flags 0x00-0x3F", Group::World, [](std::vector<uint8_t>& out) {
                         Put(out, gPlayState->actorCtx.sceneFlags.switches, 2 * sizeof(u32));
                     } });
        f.push_back({ "live switch flags 0x40-0x7F", Group::World, [](std::vector<uint8_t>& out) {
                         Put(out, &gPlayState->actorCtx.sceneFlags.switches[2], 2 * sizeof(u32));
                     } });
        f.push_back({ "live chest flags", Group::World, RAW(gPlayState->actorCtx.sceneFlags.chest) });
        f.push_back({ "live room clear flags", Group::World, [](std::vector<uint8_t>& out) {
                         const ActorContextSceneFlags& s = gPlayState->actorCtx.sceneFlags;
                         Put(out, &s.clearedRoom, sizeof(s.clearedRoom));
                         Put(out, &s.clearedRoomTemp, sizeof(s.clearedRoomTemp));
                     } });
        f.push_back({ "live collectible flags", Group::World, RAW(gPlayState->actorCtx.sceneFlags.collectible) });
        return f;
    }();
    return sFields;
}

#undef RAW

// FNV-1a
uint32_t Hash(const std::vector<uint8_t>& bytes) {
    uint32_t h = 2166136261u;
    for (uint8_t b : bytes) {
        h = (h ^ b) * 16777619u;
    }
    return h;
}

std::vector<uint32_t> HashAll() {
    const std::vector<Field>& fields = Fields();
    std::vector<uint32_t> hashes(fields.size());
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < fields.size(); i++) {
        bytes.clear();
        fields[i].read(bytes);
        hashes[i] = Hash(bytes);
    }
    return hashes;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now().time_since_epoch()).count();
}

bool InGameplay() {
    return gPlayState != nullptr && gSaveContext.gameMode == GAMEMODE_NORMAL;
}

// Host
uint32_t sCheckSeq = 0;
int64_t sNextCheckMs = 0;

// Client: per field, the time it was first seen different (0: equal), and whether its desync was already counted.
struct FieldState {
    int64_t differentSinceMs = 0;
    int checksDifferent = 0;
    bool counted = false;
};
std::vector<FieldState> sState;
int sDesyncs[3] = {};
int sChecks = 0;
int sChecksSkippedWorld = 0;
int64_t sNextStatsMs = 0;

void Drop(const char* why) {
    SPDLOG_WARN("[Coop] Dropping shared_check: {}", why);
    NoteDroppedMessage();
}

void ClientOnSharedCheck(uint32_t from, const nlohmann::json& m) {
    (void)from;
    if (IsHosting() || !IsClientPlayingInSession() || !InGameplay()) {
        return;
    }
    const std::vector<Field>& fields = Fields();
    if (!m.contains("seq") || !m["seq"].is_number_unsigned() || !m.contains("scene") ||
        !m["scene"].is_number_integer() || !m.contains("room") || !m["room"].is_number_integer() || !m.contains("h") ||
        !m["h"].is_array()) {
        Drop("missing or bad seq, scene, room or h");
        return;
    }
    const nlohmann::json& h = m["h"];
    if (h.size() != fields.size()) {
        Drop("wrong number of field hashes");
        return;
    }
    std::vector<uint32_t> theirs(fields.size());
    for (size_t i = 0; i < fields.size(); i++) {
        if (!h[i].is_number_unsigned() || h[i].get<uint64_t>() > 0xFFFFFFFFull) {
            Drop("a field hash is not a 32-bit number");
            return;
        }
        theirs[i] = h[i].get<uint32_t>();
    }
    // The live flags only compare in the same scene and room, with no room move running.
    const bool sameRoom = m["scene"].get<int64_t>() == gPlayState->sceneId &&
                          m["room"].get<int64_t>() == gPlayState->roomCtx.curRoom.num && !Rooms::MoveInProgress();
    if (!sameRoom) {
        sChecksSkippedWorld++;
    }
    sChecks++;
    sState.resize(fields.size());
    const std::vector<uint32_t> mine = HashAll();
    const int64_t now = NowMs();
    for (size_t i = 0; i < fields.size(); i++) {
        FieldState& st = sState[i];
        if (fields[i].group == Group::World && !sameRoom) {
            continue;
        }
        if (mine[i] == theirs[i]) {
            if (st.counted) {
                SPDLOG_INFO("[Coop] {}: {} matches the host again after {} ms", GroupCounter(fields[i].group),
                            fields[i].name, now - st.differentSinceMs);
            }
            st = {};
            continue;
        }
        if (st.checksDifferent++ == 0) {
            st.differentSinceMs = now;
        }
        // One check apart: a change in flight on either side is not a desync yet.
        if (st.checksDifferent >= 2 && !st.counted) {
            st.counted = true;
            sDesyncs[static_cast<int>(fields[i].group)]++;
            SPDLOG_WARN("[Coop] {}: {} differs from the host (for {} ms, check {})", GroupCounter(fields[i].group),
                        fields[i].name, now - st.differentSinceMs, m["seq"].get<uint64_t>());
        }
    }
}

void HostFrame(int64_t now) {
    if (now < sNextCheckMs || !InGameplay()) {
        return;
    }
    sNextCheckMs = now + kCheckEveryMs;
    const std::vector<uint32_t> hashes = HashAll();
    SendJson(kBroadcast, { { "type", "shared_check" },
                           { "seq", ++sCheckSeq },
                           { "scene", gPlayState->sceneId },
                           { "room", gPlayState->roomCtx.curRoom.num },
                           { "h", hashes } });
}

void LogStats(int64_t now) {
    if (now < sNextStatsMs) {
        return;
    }
    sNextStatsMs = now + kStatsEveryMs;
    if (IsHosting()) {
        SPDLOG_INFO("[Coop] shared-stats (host): checks sent {}", sCheckSeq);
        return;
    }
    int open[3] = {};
    for (size_t i = 0; i < sState.size(); i++) {
        if (sState[i].counted) {
            open[static_cast<int>(Fields()[i].group)]++;
        }
    }
    SPDLOG_INFO("[Coop] shared-stats: inv-desync {} (open {}), flag-desync {} (open {}), world-desync {} (open {}), "
                "b-save-write {}; checks {} (live flags skipped in {}: another scene or room)",
                sDesyncs[0], open[0], sDesyncs[1], open[1], sDesyncs[2], open[2], SaveWritesWhileClient(), sChecks,
                sChecksSkippedWorld);
}

void RegisterSharedHooks() {
    RegisterJsonHandler("shared_check", ClientOnSharedCheck);
}

} // namespace

void Frame() {
    SetConnectedClient(IsConnectedClient());
    if (!IsHosting() && !IsClientInSession()) {
        return;
    }
    const int64_t now = NowMs();
    if (IsHosting()) {
        HostFrame(now);
    }
    LogStats(now);
}

void TestFuzz() {
    if (!IsHosting()) {
        return;
    }
    const size_t n = Fields().size();
    auto send = [](const char* what, const nlohmann::json& m) {
        SPDLOG_INFO("[Coop] TEST: fuzz shared_check: {}", what);
        SendJson(kBroadcast, m);
    };
    std::vector<uint32_t> good(n, 0);
    send("no h", { { "type", "shared_check" }, { "seq", 1 }, { "scene", 0 }, { "room", 0 } });
    send("h is not an array", { { "type", "shared_check" }, { "seq", 1 }, { "scene", 0 }, { "room", 0 }, { "h", 7 } });
    send("one hash too few", { { "type", "shared_check" },
                               { "seq", 1 },
                               { "scene", 0 },
                               { "room", 0 },
                               { "h", std::vector<uint32_t>(n - 1, 0) } });
    send("5000 hashes", { { "type", "shared_check" },
                          { "seq", 1 },
                          { "scene", 0 },
                          { "room", 0 },
                          { "h", std::vector<uint32_t>(5000, 0) } });
    nlohmann::json bad = good;
    bad[0] = "text";
    send("a hash is text", { { "type", "shared_check" }, { "seq", 1 }, { "scene", 0 }, { "room", 0 }, { "h", bad } });
    bad[0] = -5;
    send("a negative hash", { { "type", "shared_check" }, { "seq", 1 }, { "scene", 0 }, { "room", 0 }, { "h", bad } });
    bad[0] = 0x1FFFFFFFFll;
    send("a hash over 32 bits",
         { { "type", "shared_check" }, { "seq", 1 }, { "scene", 0 }, { "room", 0 }, { "h", bad } });
    send("negative seq", { { "type", "shared_check" }, { "seq", -3 }, { "scene", 0 }, { "room", 0 }, { "h", good } });
    send("scene is text", { { "type", "shared_check" }, { "seq", 1 }, { "scene", "x" }, { "room", 0 }, { "h", good } });
}

static RegisterShipInitFunc initFunc(RegisterSharedHooks);

} // namespace Coop::Shared
