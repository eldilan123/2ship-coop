#ifndef COOP_PROJECTILE_TYPES_H
#define COOP_PROJECTILE_TYPES_H

#include <cstddef>
#include <cstdint>

extern "C" {
#include "z64actor.h"
}

// The projectile types shared between players and what each one needs, on the owner's side (when to start sending
// it, what its draw reads) and on the other side (how its copy is made inert and drawn).
//
// A copy is the same actor type, created locally, whose update never runs: it never moves by itself, never registers
// a collider that can touch anything, never reads the local Link, never spends the local player's magic and never
// writes to the save. Where a type's Init or Destroy would touch any of that, prepare() undoes or replaces it.
namespace Coop::Projectiles {

constexpr size_t kMaxExtraSize = 16;

struct TypeInfo {
    int16_t actorId;
    uint8_t extraSize;
    bool fromPlayer;      // The local Link (or one of its projectiles) may own one
    bool fromEnemy;       // Host: an enemy may spawn one; clients see it, nobody reports hits with it
    float reach;          // Hits: farthest the enemy may be from the projectile's last reported position
    uint8_t elemMaterial; // Hits: how the host's replay of a hit by this type sounds and sparks
    uint8_t atElemFlags;
    bool (*validParams)(uint16_t params);
    // Owner: whether it is shared from now on (an arrow once released, never a powder keg).
    bool (*shareable)(const Actor* actor);
    // Hits: the damage a hit by this projectile may carry (0: its hits are never reported).
    uint32_t (*hitFlags)(const Actor* actor);
    // Owner: what its draw reads besides the generic fields (extraSize bytes).
    void (*capture)(const Actor* actor, uint8_t* out);
    // Checks `in` and, if `copy` is not null, applies it. False (nothing applied) if anything is out of range.
    bool (*apply)(Actor* copy, const uint8_t* in);
    // Right after the copy's Init.
    void (*prepare)(Actor* copy);
    // Every frame while the copy is shown: what its update would animate by itself (spin, fuse sparks, sounds).
    void (*tick)(Actor* copy);
};

// nullptr if the type is not shared.
const TypeInfo* FindType(int16_t actorId);
// The game's code addresses travel as offsets from one of its functions (both peers run the same build).
int32_t CodeOffset(const void* fn);
// nullptr if the offset is not inside the game's code.
void* CodeAddress(int32_t offset);

} // namespace Coop::Projectiles

#endif // COOP_PROJECTILE_TYPES_H
