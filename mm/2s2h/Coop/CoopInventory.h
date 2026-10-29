#ifndef COOP_INVENTORY_H
#define COOP_INVENTORY_H

#include <cstdint>

// Shared inventory and economy (milestone 6, phase 1): one pool, the host's. Items, masks, bottles and their content,
// ammo, rupees, equipment and upgrades, quest items, dungeon items, keys, stray fairies, max hearts and acquired
// magic. Health, magic, form, worn mask and equipped buttons stay each player's own.
//
// The client never asks before changing its own copy: the game runs as always. At the start of each frame it compares
// its copy with the last state the host confirmed plus its own events still unconfirmed, and sends what changed as an
// inv_event: a list of [unit, from, to] for the units of the table in CoopInventory.cpp. Each unit has a kind:
//   value   (an item slot, the equipment, upgrades...): the host applies it only if it still has `from` (the first
//           event to reach the host wins; the second one is rejected and gets the resulting state);
//   counter (ammo, rupees, keys, stray fairies, max hearts...): the host adds to - from; rupees that would go below 0
//           reject the whole event (a purchase without the money), ammo below 0 stays at 0 (inv-overdraw);
//   bits    (quest items, dungeon items): the bits set and cleared are applied as they are.
// An event is applied whole or not at all. The host then sends inv_state to each client: every unit's value, the last
// event of that client it has applied and the ones it rejected. The client keeps that as the confirmed state, puts its
// unconfirmed events on top and writes the result into its save in memory, so the host's changes never come back as
// events (no echo). Nothing is written to disk (CoopSaveGuard).
//
// The scene snapshot no longer replaces the client's inventory once it is in the session; inv_state does.
//
// Game thread only.
#define CVAR_COOP_SHARED_INVENTORY "gCoop.SharedInventory" // 1 (default): one shared pool; 0: each player's own copy

namespace Coop::Inventory {

// Called once per frame from the coop frame hook.
void Frame();
// Client: the first snapshot was applied; what is in the save now is the host's confirmed state.
void OnEnteredSession();
// Client: around a later snapshot, so it leaves the inventory as it is.
void BeforeSnapshot();
void AfterSnapshot();

// Client: sends what changed this frame right away and returns that event's seq (0: nothing changed, or the shared
// inventory is off).
uint32_t SendNow();
enum class Answer { Waiting, Applied, Rejected };
// Client: the host's answer to one of this game's events.
Answer AnswerFor(uint32_t seq);
// Client: gives up on an event (inv_cancel): it is dropped here and the host undoes it if it had applied it.
void Cancel(uint32_t seq);

// Test only (host): sends malformed inv_event / inv_state messages.
void TestFuzz();
// Scenes that keep game state in their chest flags (the shooting galleries' keeper writes its score there, the
// treasure chest shop sets and clears its chest's flag every game): those flags are each game's own, neither sent
// nor checked. The overload without a scene asks about the current one.
bool ChestFlagsAreLocal(int sceneId);
// Host: the whole state goes to this client on the next frame even if nothing changed (a resync, milestone 8).
void HostResendTo(uint32_t playerId);
bool ChestFlagsAreLocal();

} // namespace Coop::Inventory

#endif // COOP_INVENTORY_H
