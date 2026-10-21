#ifndef COOP_PLAYER_HITS_H
#define COOP_PLAYER_HITS_H

#include <cstdint>
#include "CoopEnemyWire.h"

extern "C" {
#include "z64.h"
}

// Enemies hurting other players (milestone 5, phase 2). Each player keeps its own health: the host never changes a
// client's, it tells the client about the hit and the client takes it with its own Link's code.
//
// Host: a client's puppet Link registers its body cylinder (and its shield quad while it shields) for AC, unless that
// player is invulnerable, dead, in a cutscene or was just hit. When an enemy (or one of the host's shared projectiles)
// hits it, the engine gives the enemy its native AT_HIT or bounce, and a PlayerHit event goes to that client. Damage
// an enemy deals without colliders (knockback calls, damagePlayer) while it is after a client is caught by the player
// view (CoopTargeting) and sent the same way.
//
// Client: the event is checked (attacker here and near, not too old, rate) and replayed: a quad whose attacker is the
// enemy's puppet (or the projectile's copy) is placed on the local Link for one frame, so damage, knockback, effects
// (fire, ice, electric), the shield and invulnerability all come from the Link's own code.
//
// Game thread only.
#define CVAR_COOP_ENEMIES_HURT_CLIENTS "gCoop.EnemiesHurtClients" // 1: on (default); 0: no damage to clients at all
#define CVAR_COOP_PLAYER_HIT_RADIUS "gCoop.PlayerHitRadius"       // Client: attacker at most this far (600)
#define CVAR_COOP_PLAYER_HIT_MAX_AGE "gCoop.PlayerHitMaxAgeMs"    // Client: event at most this late (500)
#define CVAR_COOP_PUPPET_AC_OFF_FRAMES "gCoop.PuppetAcOffFrames"  // Host: puppet not hittable after a hit (30)

namespace Coop::PlayerHits {

// Called once per frame from the coop frame hook (after CoopCombat: replays registered last frame are read back).
void Frame();

// Host: whether a client's puppet may be hit now (its cylinder and shield are registered for AC only then).
bool MayHurt(uint32_t playerId);
// Host, from VB_PERFORM_AC_COLLISION: an AT touching a puppet's AC. Client: vetoes our replay quad on anything but
// the local Link.
void OnAcCollision(Collider* at, Collider* ac, ColliderElement* atElem, bool* should);
// Host: damage the enemy `attacker` dealt to `playerId`'s puppet without colliders while it was after that player:
// a knockback request (Player's unk_B74/B75/B76/B78/B7C) or a damagePlayer call.
void HostDirectKnockback(uint32_t playerId, Actor* attacker, u8 type, u8 damage, s16 yaw, f32 speed, f32 velocityY);
void HostDirectDamage(uint32_t playerId, Actor* attacker, s32 damage);

// Channel 5 message type for PlayerHit (host -> client), dispatched by CoopCombat.
constexpr uint8_t kMsgPlayerHit = 3;
void ClientOnPlayerHit(Enemies::Wire::Reader& r);

// Test only (both): sends malformed PlayerHit messages to the other side.
void TestFuzz();

} // namespace Coop::PlayerHits

#endif // COOP_PLAYER_HITS_H
