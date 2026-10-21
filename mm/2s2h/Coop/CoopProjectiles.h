#ifndef COOP_PROJECTILES_H
#define COOP_PROJECTILES_H

#include <cstdint>

extern "C" {
#include "z64.h"
}

// Projectiles shared between players (binary channel kChannelProjectiles, see CoopProjectileWire.h).
//
// Each player owns the projectiles its own Link fires (and what they spawn: a magic arrow's effect, a Deku nut's
// flash, a bombchu's blast); the host also owns the ones its enemies spawn. The owner simulates them and sends where
// they are; everyone else draws a copy: the same actor type, created locally, whose update never runs and whose
// colliders never touch anything (CoopProjectileTypes). Only the owner ends a projectile: its copy goes away when
// the owner's Kill arrives (on the host, only after every hit sent for it was applied).
//
// Hits: a client's projectile that hits an enemy puppet becomes a hit event on channel 5 (CoopCombat), and the host
// replays it with the copy as the attacker, so the enemy reacts to the arrow or the bomb itself.
//
// Game thread only.
namespace Coop::Projectiles {

// Called once per frame from the coop frame hook, after CoopCombat (hits found this frame are sent before the Kill
// of the projectile that made them).
void Frame();

// Owner: the shared id of a projectile this game owns, sending its Spawn first if it had not been sent; 0 if
// `actor` is not one (or cannot be shared: over the cap).
uint16_t OwnedId(Actor* actor);
// Owner: a hit event was sent for this projectile (counted in its Kill).
void NoteHitSent(uint16_t id);
// The damage a hit by this owned projectile may carry, 0 if none.
uint32_t HitFlags(const Actor* actor);
// Any side: whether `actor` is the copy of another player's projectile.
bool IsCopy(const Actor* actor);

// Host: the copy of `owner`'s projectile `id`, for replaying a hit by it.
struct CopyView {
    Actor* actor = nullptr;
    uint32_t hitFlags = 0; // What a hit by it may carry
    float reach = 0.0f;    // Farthest the enemy may be from lastPos
    Vec3f lastPos = {};    // Newest position the owner reported
    uint8_t elemMaterial = 0;
    uint8_t atElemFlags = 0;
};
bool HostCopy(uint32_t owner, uint16_t id, CopyView& out);
// Host: every hit event received for the projectile (applied or not) counts against the hitsSent of its Kill;
// the copy is kept while hits for it wait in the queue or are being replayed.
void HostHitArrived(uint32_t owner, uint16_t id);
void HostHitQueued(uint32_t owner, uint16_t id);
void HostHitFinished(uint32_t owner, uint16_t id);

// Test only (both): fires a projectile from the local Link at the nearest enemy, as the Link would (it is owned and
// shared like one). `kind`: arrow, fire, ice, light, bubble, nut, bomb, chu, fins, bomblong (a bomb with its whole
// fuse, dropped in front of the Link).
void TestShoot(const char* kind);
// Test only (both): sends malformed projectile messages to the other side.
void TestFuzz();
// Test only (both): asks the explosive limit what Player asks before taking out a bomb, and logs the answer.
void TestBombLimit();

} // namespace Coop::Projectiles

#endif // COOP_PROJECTILES_H
