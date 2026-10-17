#ifndef COOP_ENEMIES_H
#define COOP_ENEMIES_H

#include <cstdint>

// Enemy sync (binary channel kChannelEnemies, see CoopEnemyWire.h). The host simulates every enemy (category
// ACTORCAT_ENEMY) and gives each one a netId; on a client in the session every enemy is a puppet: its update never
// runs, it draws the host's pose and is removed only when the host says so. Puppets neither deal nor take damage
// (no colliders are registered without the update). Game thread only.
namespace Coop::Enemies {

// Called once per frame from the coop frame hook.
void Frame();

// Test only (host): spawns an enemy in front of the local Link. `index` spreads several spawns made at once.
void TestSpawn(int16_t actorId, uint16_t params, int index);
// Test only (host): kills the enemy with this netId (-1: the newest one still alive).
void TestKill(int netId);
// Test only (host), one step of --coop-test-sweep: removes every enemy spawned at run time (the previous step's,
// with its minions and projectiles), then spawns this one and logs what became of it.
void TestSweepStep(int step, int total, int16_t actorId, uint16_t params);
// Test only (both): sends malformed channel messages to the other side, then a wrong summary (client: a request).
void TestFuzz();

} // namespace Coop::Enemies

#endif // COOP_ENEMIES_H
