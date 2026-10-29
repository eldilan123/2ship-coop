#ifndef COOP_SHARED_H
#define COOP_SHARED_H

#include <cstdint>

// Shared state (milestone 6): the host's save is the one both players play on. Inventory, rupees, ammo, bottles, max
// hearts, story flags and scene flags are shared; health, magic, form, worn mask and equipped buttons are each
// player's own.
//
// Phase 1, first step: only a detector, nothing is changed. Every 2 s the host sends shared_check with a hash of each
// shared field (CoopShared.cpp has the table) and the scene and room it is in. The client hashes its own copy and,
// when a field still differs on the next check, logs it and counts one desync: inv-desync (inventory and economy),
// flag-desync (story and saved scene flags) or world-desync (the live flags of the scene, only while both are in the
// same scene and room). A field that matches again is logged as resolved. Every 10 s: shared-stats.
//
// Game thread only.
namespace Coop::Shared {

// Called once per frame from the coop frame hook.
void Frame();

// Client: a resync was applied (milestone 8): every field starts over. A field that stays different for 3 checks
// (6 s) asks for a resync by itself, at most once every 30 s (auto-resync in shared-stats).
void OnResynced();

// Test only (host): sends malformed shared_check messages to the clients.
void TestFuzz();

} // namespace Coop::Shared

#endif // COOP_SHARED_H
