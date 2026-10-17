#ifndef COOP_ENEMY_TYPES_H
#define COOP_ENEMY_TYPES_H

#include <cstddef>
#include <cstdint>

extern "C" {
#include "z64actor.h"
}

// Per enemy type knowledge for enemy puppets: what the type's draw code reads besides the skeleton and the generic
// Actor fields. Types listed here can also be created on a client by a host message.
namespace Coop::Enemies {

constexpr size_t kMaxExtraSize = 64;

struct TypeInfo {
    int16_t actorId;
    const char* name;
    uint8_t extraSize;
    // Copies the draw-only fields into `out` (extraSize bytes).
    void (*capture)(const Actor* actor, uint8_t* out);
    // Checks `in` (extraSize bytes from the network) and, if `actor` is not null, applies it. False (nothing
    // applied) if any field is out of range.
    bool (*apply)(Actor* actor, const uint8_t* in);
    // Whether a host message may create this type with these params (the draw code indexes tables with them).
    bool (*validParams)(uint16_t params);
    // Optional: what the type's own update would have set up before it can be drawn (run once, on the puppet).
    void (*prepare)(Actor* actor);
};

// nullptr if the type has no entry.
const TypeInfo* FindType(int16_t actorId);
// Where an enemy type keeps its actionFunc (many enemy draws compare it), or -1. Generated for every enemy type
// (CoopEnemyActionFuncs.inc). On a puppet the host's value is only ever compared: the puppet's update never runs.
ptrdiff_t ActionFuncOffset(int16_t actorId);
// The actor's name from the actor table for logs ("En_Wf"), or "actor 0x..." for an unknown id.
const char* TypeName(int16_t actorId);
// Its human readable description from the actor table ("Wolfos/White Wolfos"), or "".
const char* TypeDescription(int16_t actorId);
// Whether a host message may create this type on a client: an enemy (by profile, or switching to ENEMY in its
// Init), not a boss, and not known to misbehave as a puppet. Types without an entry are drawn with the generic
// fields only (pose, placement, scale, color filter).
// `withParent`: the host spawned it from another enemy and the client has that enemy's puppet.
bool CanCreateOnClient(int16_t actorId, bool withParent, const char*& why);

} // namespace Coop::Enemies

#endif // COOP_ENEMY_TYPES_H
