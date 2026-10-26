#ifndef COOP_ENEMIES_H
#define COOP_ENEMIES_H

#include <cstddef>
#include <cstdint>
#include <functional>

extern "C" {
#include "z64.h"
}

// Enemy sync (binary channel kChannelEnemies, see CoopEnemyWire.h). The host simulates every enemy (category
// ACTORCAT_ENEMY) and gives each one a netId; on a client in the session every enemy is a puppet: its update never
// runs, it draws the host's pose and is removed only when the host says so. Puppets neither deal nor take damage
// (no colliders are registered without the update). Game thread only.
namespace Coop::Enemies {

// Called once per frame from the coop frame hook.
void Frame();

// For hits on enemies (CoopCombat). Host: the live enemy with this netId, or null; the netId of an enemy, or 0.
Actor* HostEnemyActor(uint16_t netId);
uint16_t HostNetIdOf(const Actor* actor);
// Client: the colliders registered for AC on each shown puppet in the previous frame (before this frame's
// registration replaces them), with the puppet's netId.
void ForEachRegisteredPuppetCollider(const std::function<void(uint16_t netId, Actor* actor, Collider* col)>& fn);
// Client: the shown puppet with this netId, or null.
Actor* PuppetActor(uint16_t netId);
// Host: tracked enemies alive in this room (or in no room). Client: puppets shown, and netIds with more than one
// puppet actor (must be 0). Room move checks (CoopRooms).
size_t HostEnemiesInRoom(int8_t room);
void ClientPuppetCounts(size_t& shown, size_t& duplicateNetIds);
// Client: the netId of an enemy puppet, or 0.
uint16_t PuppetNetIdOf(const Actor* actor);
// The nearest live enemy (host) or shown puppet (client) to `from`, or null. `testFirst` (tests): enemies spawned at
// run time are preferred over room enemies.
Actor* NearestEnemy(const Vec3f& from, bool testFirst = true);

// Test only (host): spawns an enemy in front of the local Link. `index` spreads several spawns made at once.
void TestSpawn(int16_t actorId, uint16_t params, int index);
// Test only (host): kills the enemy with this netId (-1: the newest one still alive; -2: every enemy spawned at
// run time).
void TestKill(int netId);
// Test only (host), one step of --coop-test-sweep: removes every enemy spawned at run time (the previous step's,
// with its minions and projectiles), then spawns this one and logs what became of it.
void TestSweepStep(int step, int total, int16_t actorId, uint16_t params);
// Test only (both): sends malformed channel messages to the other side, then a wrong summary (client: a request).
void TestFuzz();

} // namespace Coop::Enemies

#endif // COOP_ENEMIES_H
