#ifndef COOP_COMBAT_H
#define COOP_COMBAT_H

#include <cstddef>
#include <cstdint>
#include <vector>

// A client's hits on enemy puppets (binary channel kChannelHits).
//
// Client: the puppets' colliders are registered for AC from the host's description (CoopEnemyColliders), so the
// client's own collision check resolves its Link's attacks against them: the local Link gets its normal feedback
// (hit sound, hit-stop, recoil on hard surfaces). Each hit becomes an event to the host. The puppet itself never
// changes: its update does not run and its health is the host's.
//
// Host: checks the event, then replays the hit natively. One of the weapon quads of that player's puppet Link is
// placed at the hit point on the real enemy and registered as an AT collider before this frame's collision check,
// so the engine resolves it against whatever the enemy registered (invulnerable, shielded, hard...) and the enemy
// reacts in its own update exactly as to a hit from the host's Link. The result goes back to the client.
//
// Game thread only.
#define CVAR_COOP_PREDICT_HIT_FLASH "gCoop.PredictHitFlash"

namespace Coop::Combat {

// Called once per frame from the coop frame hook, before the enemy puppets register their colliders again.
void Frame();
// Test only (both): sends malformed hit messages to the other side.
void TestFuzz();
// A message of the hits channel (magic, format and `type`), for the other modules that use it.
std::vector<uint8_t> HitsMessage(uint8_t type, const void* body, size_t size);
// Test only (host): hits the nearest enemy once with these damage flags, as the local Link would (resolved by the
// engine against whatever the enemy registered).
void TestHitNearest(uint32_t dmgFlags);

} // namespace Coop::Combat

#endif // COOP_COMBAT_H
