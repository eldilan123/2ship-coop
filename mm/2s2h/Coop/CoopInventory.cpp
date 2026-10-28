#include "CoopInventory.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <string>
#include <vector>
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include "Coop.h"
#include "CoopSession.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
}

namespace Coop::Inventory {

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr int64_t kStatsEveryMs = 10000;
constexpr size_t kMaxOps = 512;

enum class Kind { Value, Counter, Bits };

const char* KindName(Kind k) {
    switch (k) {
        case Kind::Value:
            return "value";
        case Kind::Counter:
            return "counter";
        case Kind::Bits:
            return "bits";
    }
    return "?";
}

// One shared unit of the save. `min`/`max` bound what a message may carry (and, for counters, the result).
struct Unit {
    std::string name;
    Kind kind;
    int64_t min;
    int64_t max;
    int64_t (*get)(int index);
    void (*set)(int index, int64_t value);
    int index;
    int itemSlot; // >= 0: an item slot (equipped buttons follow it)
};

::Inventory& Inv() {
    return gSaveContext.save.saveInfo.inventory;
}
SavePlayerData& Pd() {
    return gSaveContext.save.saveInfo.playerData;
}

constexpr u32 kQuestBitsMask = 0x0FFFFFFF; // Bits 28-31 are the heart piece count

const std::vector<Unit>& Units() {
    static const std::vector<Unit> sUnits = [] {
        std::vector<Unit> u;
        for (int i = 0; i < ARRAY_COUNT(Inv().items); i++) {
            u.push_back({ fmt::format("items[{}]", i), Kind::Value, 0, 0xFF,
                          [](int k) -> int64_t { return Inv().items[k]; },
                          [](int k, int64_t v) { Inv().items[k] = static_cast<u8>(v); }, i, i });
        }
        for (int i = 0; i < ARRAY_COUNT(Inv().ammo); i++) {
            u.push_back({ fmt::format("ammo[{}]", i), Kind::Counter, 0, 127,
                          [](int k) -> int64_t { return Inv().ammo[k]; },
                          [](int k, int64_t v) { Inv().ammo[k] = static_cast<s8>(v); }, i, -1 });
        }
        // Rupees still in the accumulator count: Interface_Update moves them a few per frame, and never below 0 or
        // past the wallet.
        u.push_back(
            { "rupees", Kind::Counter, 0, 999,
              [](int) -> int64_t {
                  return std::clamp<int64_t>(Pd().rupees + gSaveContext.rupeeAccumulator, 0, CUR_CAPACITY(UPG_WALLET));
              },
              [](int, int64_t v) { Pd().rupees = static_cast<s16>(v - gSaveContext.rupeeAccumulator); }, 0, -1 });
        u.push_back({ "bank rupees", Kind::Counter, 0, 0xFFFF, [](int) -> int64_t { return HS_GET_BANK_RUPEES(); },
                      [](int, int64_t v) { HS_SET_BANK_RUPEES(static_cast<u32>(v)); }, 0, -1 });
        u.push_back({ "equipment", Kind::Value, 0, 0xFFFF,
                      [](int) -> int64_t { return gSaveContext.save.saveInfo.equips.equipment; },
                      [](int, int64_t v) { gSaveContext.save.saveInfo.equips.equipment = static_cast<u16>(v); }, 0,
                      -1 });
        u.push_back({ "upgrades", Kind::Value, 0, 0xFFFFFFFFll, [](int) -> int64_t { return Inv().upgrades; },
                      [](int, int64_t v) { Inv().upgrades = static_cast<u32>(v); }, 0, -1 });
        u.push_back({ "questItems", Kind::Bits, 0, kQuestBitsMask,
                      [](int) -> int64_t { return Inv().questItems & kQuestBitsMask; },
                      [](int, int64_t v) {
                          Inv().questItems =
                              (Inv().questItems & ~kQuestBitsMask) | (static_cast<u32>(v) & kQuestBitsMask);
                      },
                      0, -1 });
        u.push_back({ "heart pieces", Kind::Value, 0, 15, [](int) -> int64_t { return Inv().questItems >> 28; },
                      [](int, int64_t v) {
                          Inv().questItems = (Inv().questItems & kQuestBitsMask) | (static_cast<u32>(v) << 28);
                      },
                      0, -1 });
        for (int i = 0; i < ARRAY_COUNT(Inv().dungeonItems); i++) {
            u.push_back({ fmt::format("dungeonItems[{}]", i), Kind::Bits, 0, 0xFF,
                          [](int k) -> int64_t { return Inv().dungeonItems[k]; },
                          [](int k, int64_t v) { Inv().dungeonItems[k] = static_cast<u8>(v); }, i, -1 });
        }
        for (int i = 0; i < ARRAY_COUNT(Inv().dungeonKeys); i++) {
            u.push_back({ fmt::format("dungeonKeys[{}]", i), Kind::Counter, -1, 127,
                          [](int k) -> int64_t { return Inv().dungeonKeys[k]; },
                          [](int k, int64_t v) { Inv().dungeonKeys[k] = static_cast<s8>(v); }, i, -1 });
        }
        for (int i = 0; i < ARRAY_COUNT(Inv().strayFairies); i++) {
            u.push_back({ fmt::format("strayFairies[{}]", i), Kind::Counter, 0, 127,
                          [](int k) -> int64_t { return Inv().strayFairies[k]; },
                          [](int k, int64_t v) { Inv().strayFairies[k] = static_cast<s8>(v); }, i, -1 });
        }
        u.push_back({ "defenseHearts", Kind::Value, -128, 127, [](int) -> int64_t { return Inv().defenseHearts; },
                      [](int, int64_t v) { Inv().defenseHearts = static_cast<s8>(v); }, 0, -1 });
        u.push_back({ "healthCapacity", Kind::Counter, 0, 0x7FFF, [](int) -> int64_t { return Pd().healthCapacity; },
                      [](int, int64_t v) { Pd().healthCapacity = static_cast<s16>(v); }, 0, -1 });
        u.push_back({ "isMagicAcquired", Kind::Value, 0, 0xFF, [](int) -> int64_t { return Pd().isMagicAcquired; },
                      [](int, int64_t v) { Pd().isMagicAcquired = static_cast<u8>(v); }, 0, -1 });
        u.push_back({ "isDoubleMagicAcquired", Kind::Value, 0, 0xFF,
                      [](int) -> int64_t { return Pd().isDoubleMagicAcquired; },
                      [](int, int64_t v) { Pd().isDoubleMagicAcquired = static_cast<u8>(v); }, 0, -1 });
        u.push_back({ "doubleDefense", Kind::Value, 0, 0xFF, [](int) -> int64_t { return Pd().doubleDefense; },
                      [](int, int64_t v) { Pd().doubleDefense = static_cast<u8>(v); }, 0, -1 });
        u.push_back({ "swordHealth", Kind::Counter, 0, 0xFFFF, [](int) -> int64_t { return Pd().swordHealth; },
                      [](int, int64_t v) { Pd().swordHealth = static_cast<u16>(v); }, 0, -1 });
        u.push_back({ "skullTokenCount", Kind::Value, 0, 0xFFFFFFFFll,
                      [](int) -> int64_t { return gSaveContext.save.saveInfo.skullTokenCount; },
                      [](int, int64_t v) { gSaveContext.save.saveInfo.skullTokenCount = static_cast<u32>(v); }, 0,
                      -1 });
        u.push_back({ "stolenItems", Kind::Value, 0, 0xFFFFFFFFll,
                      [](int) -> int64_t { return gSaveContext.save.saveInfo.stolenItems; },
                      [](int, int64_t v) { gSaveContext.save.saveInfo.stolenItems = static_cast<u32>(v); }, 0, -1 });
        return u;
    }();
    return sUnits;
}

std::vector<int64_t> ReadAll() {
    const std::vector<Unit>& units = Units();
    std::vector<int64_t> v(units.size());
    for (size_t i = 0; i < units.size(); i++) {
        v[i] = units[i].get(units[i].index);
    }
    return v;
}

struct Op {
    size_t unit;
    int64_t from;
    int64_t to;
};

// What an op does to a value it is applied on (the host's, or the client's confirmed state plus its events).
int64_t ApplyOp(const Unit& u, int64_t cur, const Op& op) {
    switch (u.kind) {
        case Kind::Value:
            return op.to;
        case Kind::Counter:
            return std::clamp<int64_t>(cur + (op.to - op.from), u.min, u.max);
        case Kind::Bits: {
            const int64_t set = op.to & ~op.from;
            const int64_t clear = op.from & ~op.to;
            return (cur | set) & ~clear;
        }
    }
    return cur;
}

std::string Describe(const std::vector<Op>& ops) {
    const std::vector<Unit>& units = Units();
    std::string s;
    for (size_t i = 0; i < ops.size() && i < 8; i++) {
        const Op& op = ops[i];
        const Unit& u = units[op.unit];
        if (!s.empty()) {
            s += ", ";
        }
        if (u.kind == Kind::Counter) {
            s += fmt::format("{} {:+}", u.name, op.to - op.from);
        } else {
            s += fmt::format("{} 0x{:X} -> 0x{:X}", u.name, op.from, op.to);
        }
    }
    if (ops.size() > 8) {
        s += fmt::format(", ... ({} ops)", ops.size());
    }
    return s;
}

bool Enabled() {
    return CVarGetInteger(CVAR_COOP_SHARED_INVENTORY, 1) != 0;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now().time_since_epoch()).count();
}

// An item slot changed under this player's equipped buttons: they show the new content (each player's buttons are
// its own; the game does the same in Inventory_UpdateBottleItem / Inventory_DeleteItem).
void RefreshButtonsForSlot(int slot) {
    if (gPlayState == nullptr) {
        return;
    }
    const u8 item = Inv().items[slot];
    for (u8 btn = EQUIP_SLOT_C_LEFT; btn <= EQUIP_SLOT_C_RIGHT; btn++) {
        if (C_SLOT_EQUIP(0, btn) == slot && BUTTON_ITEM_EQUIP(0, btn) != item) {
            BUTTON_ITEM_EQUIP(0, btn) = item;
            if (item == ITEM_NONE) {
                C_SLOT_EQUIP(0, btn) = SLOT_NONE;
            }
            Interface_LoadItemIconImpl(gPlayState, btn);
        }
    }
    for (u8 btn = EQUIP_SLOT_D_RIGHT; btn <= EQUIP_SLOT_D_UP; btn++) {
        if (DPAD_GET_CUR_FORM_BTN_SLOT(btn) == slot && DPAD_BUTTON_ITEM_EQUIP(0, btn) != item) {
            DPAD_BUTTON_ITEM_EQUIP(0, btn) = item;
            if (item == ITEM_NONE) {
                DPAD_SLOT_EQUIP(0, btn) = SLOT_NONE;
            }
            Interface_Dpad_LoadItemIconImpl(gPlayState, btn);
        }
    }
}

// The shared sword changed: this player's B button (human form) shows it, as Item_GiveImpl does for the one who got it.
void RefreshSwordButton() {
    if (gPlayState == nullptr) {
        return;
    }
    const u8 cur = BUTTON_ITEM_EQUIP(0, EQUIP_SLOT_B);
    const bool holdsSword = (cur >= ITEM_SWORD_KOKIRI && cur <= ITEM_SWORD_GILDED) || cur == ITEM_NONE;
    const s32 sword = GET_CUR_EQUIP_VALUE(EQUIP_TYPE_SWORD);
    if (!holdsSword || sword == EQUIP_VALUE_SWORD_NONE || sword > EQUIP_VALUE_SWORD_GILDED) {
        return;
    }
    const u8 item = static_cast<u8>(ITEM_SWORD_KOKIRI + sword - EQUIP_VALUE_SWORD_KOKIRI);
    if (cur != item) {
        BUTTON_ITEM_EQUIP(0, EQUIP_SLOT_B) = item;
        Interface_LoadItemIconImpl(gPlayState, EQUIP_SLOT_B);
    }
}

// Writes `values` into the save where they differ, then refreshes this player's buttons.
void WriteAll(const std::vector<int64_t>& values) {
    const std::vector<Unit>& units = Units();
    const std::vector<int64_t> live = ReadAll();
    bool swordChanged = false;
    for (size_t i = 0; i < units.size(); i++) {
        if (live[i] == values[i]) {
            continue;
        }
        units[i].set(units[i].index, values[i]);
        if (units[i].itemSlot >= 0) {
            RefreshButtonsForSlot(units[i].itemSlot);
        }
        if (units[i].name == "equipment") {
            swordChanged = true;
        }
    }
    if (swordChanged) {
        RefreshSwordButton();
    }
}

// MARK: - Host

struct ClientRecord {
    uint32_t lastSeq = 0;
    bool ackPending = true;
    std::vector<std::pair<uint32_t, std::string>> rejected;
};
std::map<uint32_t, ClientRecord> sClients;
std::vector<int64_t> sLastSent;
int sApplied = 0;
int sRejected = 0;
int sOverdraws = 0;
int sStatesSent = 0;

void Drop(const char* what, const char* why) {
    SPDLOG_WARN("[Coop] Dropping {}: {}", what, why);
    NoteDroppedMessage();
}

bool ParseOps(const nlohmann::json& j, std::vector<Op>& ops, const char** why) {
    const std::vector<Unit>& units = Units();
    if (!j.is_array() || j.empty() || j.size() > kMaxOps) {
        *why = "ops is not a list of 1 to 512 ops";
        return false;
    }
    for (const nlohmann::json& o : j) {
        if (!o.is_array() || o.size() != 3 || !o[0].is_number_unsigned() || !o[1].is_number_integer() ||
            !o[2].is_number_integer()) {
            *why = "an op is not [unit, from, to]";
            return false;
        }
        const uint64_t unit = o[0].get<uint64_t>();
        if (unit >= units.size()) {
            *why = "an op names a unit that does not exist";
            return false;
        }
        const Unit& u = units[unit];
        const int64_t from = o[1].get<int64_t>(), to = o[2].get<int64_t>();
        // A counter carries a change: the client's own copy may have gone past the range (spending rupees it does
        // not have in the pool); the host decides on the result.
        const int64_t slack = u.kind == Kind::Counter ? 0xFFFF : 0;
        if (from < u.min - slack || from > u.max + slack || to < u.min - slack || to > u.max + slack || from == to) {
            *why = "an op's value is out of range for its unit (or changes nothing)";
            return false;
        }
        ops.push_back({ static_cast<size_t>(unit), from, to });
    }
    return true;
}

void HostOnInvEvent(uint32_t from, const nlohmann::json& m) {
    if (!IsHosting() || !Enabled() || gPlayState == nullptr) {
        return;
    }
    if (!m.contains("seq") || !m["seq"].is_number_unsigned() || !m.contains("ops")) {
        Drop("inv_event", "missing or bad seq or ops");
        return;
    }
    const uint64_t seq64 = m["seq"].get<uint64_t>();
    ClientRecord& c = sClients[from];
    if (seq64 == 0 || seq64 > 0xFFFFFFFFull || seq64 <= c.lastSeq) {
        Drop("inv_event", "seq is 0, too big or not newer than the last one");
        return;
    }
    std::vector<Op> ops;
    const char* why = nullptr;
    if (!ParseOps(m["ops"], ops, &why)) {
        Drop("inv_event", why);
        return;
    }
    const uint32_t seq = static_cast<uint32_t>(seq64);
    c.lastSeq = seq;
    c.ackPending = true;
    // All or nothing: check every op against the host's values, then write.
    const std::vector<Unit>& units = Units();
    std::vector<int64_t> values = ReadAll();
    std::string reject;
    std::vector<std::string> overdraws;
    for (const Op& op : ops) {
        const Unit& u = units[op.unit];
        const int64_t cur = values[op.unit];
        if (u.kind == Kind::Value && cur != op.from) {
            reject = fmt::format("{} is 0x{:X} on the host, not 0x{:X} (another change got there first)", u.name, cur,
                                 op.from);
            break;
        }
        if (u.kind == Kind::Counter) {
            const int64_t result = cur + (op.to - op.from);
            if (result < u.min && u.name == "rupees") {
                reject = fmt::format("rupees: the host has {}, not enough for {}", cur, op.to - op.from);
                break;
            }
            if (result < u.min || result > u.max) {
                overdraws.push_back(fmt::format("{} {} {:+} -> {}", u.name, cur, op.to - op.from,
                                                std::clamp<int64_t>(result, u.min, u.max)));
            }
        }
        values[op.unit] = ApplyOp(u, cur, op);
    }
    if (!reject.empty()) {
        sRejected++;
        c.rejected.push_back({ seq, reject });
        SPDLOG_INFO("[Coop] inv-reject: player {} seq {} ({}): {}", from, seq, Describe(ops), reject);
        return;
    }
    WriteAll(values);
    sApplied++;
    for (const std::string& o : overdraws) {
        sOverdraws++;
        SPDLOG_INFO("[Coop] inv-overdraw: player {} seq {}: {} (kept within the unit's range)", from, seq, o);
    }
    SPDLOG_INFO("[Coop] inv-event: player {} seq {} applied: {}", from, seq, Describe(ops));
}

void HostFrame() {
    const std::vector<int64_t> values = ReadAll();
    const bool changed = values != sLastSent;
    for (const PlayerInfo& p : GetSnapshot().players) {
        if (p.isHost) {
            continue;
        }
        ClientRecord& c = sClients[p.id];
        if (!changed && !c.ackPending) {
            continue;
        }
        nlohmann::json rejected = nlohmann::json::array();
        for (const auto& [seq, why] : c.rejected) {
            rejected.push_back({ seq, why });
        }
        SendJson(p.id, { { "type", "inv_state" }, { "ack", c.lastSeq }, { "rejected", rejected }, { "v", values } });
        c.ackPending = false;
        c.rejected.clear();
        sStatesSent++;
    }
    sLastSent = values;
}

// MARK: - Client

struct Pending {
    uint32_t seq;
    std::vector<Op> ops;
};
bool sHaveAuth = false;
std::vector<int64_t> sAuth;
std::deque<Pending> sPending;
uint32_t sNextSeq = 0;
int sSent = 0;
int sAcked = 0;
int sRejectedHere = 0;
std::vector<int64_t> sKept; // Around a snapshot

std::vector<int64_t> Expected() {
    const std::vector<Unit>& units = Units();
    std::vector<int64_t> v = sAuth;
    for (const Pending& p : sPending) {
        for (const Op& op : p.ops) {
            v[op.unit] = ApplyOp(units[op.unit], v[op.unit], op);
        }
    }
    return v;
}

// What changed in this game since the last look goes to the host as one event.
void ClientCapture() {
    if (!sHaveAuth) {
        return;
    }
    const std::vector<int64_t> expected = Expected();
    const std::vector<int64_t> live = ReadAll();
    std::vector<Op> ops;
    for (size_t i = 0; i < live.size(); i++) {
        if (live[i] != expected[i]) {
            ops.push_back({ i, expected[i], live[i] });
        }
    }
    if (ops.empty()) {
        return;
    }
    const uint32_t seq = ++sNextSeq;
    nlohmann::json j = nlohmann::json::array();
    for (const Op& op : ops) {
        j.push_back({ op.unit, op.from, op.to });
    }
    SendJson(kHostPlayerId, { { "type", "inv_event" }, { "seq", seq }, { "ops", j } });
    SPDLOG_INFO("[Coop] inv-event: seq {} sent: {}", seq, Describe(ops));
    sPending.push_back({ seq, std::move(ops) });
    sSent++;
}

void ClientOnInvState(uint32_t, const nlohmann::json& m) {
    if (IsHosting() || !Enabled() || !IsClientInSession() || !sHaveAuth) {
        return;
    }
    const std::vector<Unit>& units = Units();
    if (!m.contains("ack") || !m["ack"].is_number_unsigned() || !m.contains("rejected") || !m["rejected"].is_array() ||
        !m.contains("v") || !m["v"].is_array() || m["v"].size() != units.size()) {
        Drop("inv_state", "missing or bad ack, rejected or v");
        return;
    }
    std::vector<int64_t> values(units.size());
    for (size_t i = 0; i < units.size(); i++) {
        const nlohmann::json& x = m["v"][i];
        if (!x.is_number_integer() || x.get<int64_t>() < units[i].min || x.get<int64_t>() > units[i].max) {
            Drop("inv_state", "a value is out of range for its unit");
            return;
        }
        values[i] = x.get<int64_t>();
    }
    std::vector<std::pair<uint32_t, std::string>> rejected;
    for (const nlohmann::json& r : m["rejected"]) {
        if (!r.is_array() || r.size() != 2 || !r[0].is_number_unsigned() || !r[1].is_string()) {
            Drop("inv_state", "a rejected entry is not [seq, reason]");
            return;
        }
        rejected.push_back({ r[0].get<uint32_t>(), r[1].get<std::string>() });
    }
    // This game's changes since the start of the frame go out first, so writing the host's state does not lose them.
    ClientCapture();
    const uint64_t ack = m["ack"].get<uint64_t>();
    for (const auto& [seq, why] : rejected) {
        sRejectedHere++;
        SPDLOG_INFO("[Coop] inv-reject: seq {} rejected by the host: {}", seq, why);
    }
    while (!sPending.empty() && sPending.front().seq <= ack) {
        sPending.pop_front();
        sAcked++;
    }
    sAuth = values;
    WriteAll(Expected());
}

void LogStats() {
    static int64_t sNext = 0;
    const int64_t now = NowMs();
    if (now < sNext) {
        return;
    }
    sNext = now + kStatsEveryMs;
    if (IsHosting()) {
        SPDLOG_INFO("[Coop] inv-stats (host): events applied {}, rejected {}, overdraws {}; states sent {}", sApplied,
                    sRejected, sOverdraws, sStatesSent);
    } else {
        SPDLOG_INFO("[Coop] inv-stats: events sent {}, confirmed {}, rejected {}, waiting {}", sSent, sAcked,
                    sRejectedHere, sPending.size());
    }
}

void RegisterInventoryHooks() {
    RegisterJsonHandler("inv_event", HostOnInvEvent);
    RegisterJsonHandler("inv_state", ClientOnInvState);
}

} // namespace

void Frame() {
    if (!Enabled()) {
        sHaveAuth = false;
        return;
    }
    if (IsHosting()) {
        if (gPlayState != nullptr) {
            HostFrame();
        }
        LogStats();
        return;
    }
    sClients.clear();
    sLastSent.clear();
    if (!IsClientInSession()) {
        sHaveAuth = false;
        sPending.clear();
        return;
    }
    ClientCapture();
    LogStats();
}

void OnEnteredSession() {
    sAuth = ReadAll();
    sPending.clear();
    sHaveAuth = Enabled();
    SPDLOG_INFO("[Coop] inv: the host's inventory is the shared pool ({} units)", sAuth.size());
}

void BeforeSnapshot() {
    sKept.clear();
    if (!sHaveAuth) {
        return;
    }
    ClientCapture();
    sKept = ReadAll();
}

void AfterSnapshot() {
    if (sHaveAuth && sKept.size() == Units().size()) {
        WriteAll(sKept);
    }
    sKept.clear();
}

void TestFuzz() {
    const size_t n = Units().size();
    auto send = [](uint32_t to, const char* what, const nlohmann::json& m) {
        SPDLOG_INFO("[Coop] TEST: fuzz {}: {}", m["type"].get<std::string>(), what);
        SendJson(to, m);
    };
    if (IsHosting()) {
        std::vector<int64_t> v = ReadAll();
        send(kBroadcast, "no v", { { "type", "inv_state" }, { "ack", 0 }, { "rejected", nlohmann::json::array() } });
        send(kBroadcast, "one value too few",
             { { "type", "inv_state" },
               { "ack", 0 },
               { "rejected", nlohmann::json::array() },
               { "v", std::vector<int64_t>(n - 1, 0) } });
        nlohmann::json bad = v;
        bad[0] = 5000; // items[0] is a byte
        send(kBroadcast, "an item slot of 5000",
             { { "type", "inv_state" }, { "ack", 0 }, { "rejected", nlohmann::json::array() }, { "v", bad } });
        send(kBroadcast, "rejected entry without a reason",
             { { "type", "inv_state" }, { "ack", 0 }, { "rejected", { { 3 } } }, { "v", v } });
        send(kBroadcast, "ack is negative",
             { { "type", "inv_state" }, { "ack", -1 }, { "rejected", nlohmann::json::array() }, { "v", v } });
    } else if (IsClientInSession()) {
        send(kHostPlayerId, "no ops", { { "type", "inv_event" }, { "seq", 100000 } });
        send(kHostPlayerId, "seq 0", { { "type", "inv_event" }, { "seq", 0 }, { "ops", { { 0, 0, 1 } } } });
        send(kHostPlayerId, "unit 9999", { { "type", "inv_event" }, { "seq", 100001 }, { "ops", { { 9999, 0, 1 } } } });
        send(kHostPlayerId, "item slot to 300",
             { { "type", "inv_event" }, { "seq", 100002 }, { "ops", { { 0, 0, 300 } } } });
        send(kHostPlayerId, "an op of two numbers",
             { { "type", "inv_event" }, { "seq", 100003 }, { "ops", { { 0, 1 } } } });
        send(kHostPlayerId, "an op that changes nothing",
             { { "type", "inv_event" }, { "seq", 100004 }, { "ops", { { 0, 7, 7 } } } });
        send(
            kHostPlayerId, "600 ops",
            { { "type", "inv_event" }, { "seq", 100005 }, { "ops", std::vector<std::vector<int>>(600, { 0, 0, 1 }) } });
    }
}

static RegisterShipInitFunc initFunc(RegisterInventoryHooks);

} // namespace Coop::Inventory
