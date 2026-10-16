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
    // Where the type keeps its actionFunc (its draw compares it), or -1. On a puppet it is only ever compared:
    // the puppet's update never runs.
    ptrdiff_t actionFuncOffset;
    uint8_t extraSize;
    // Copies the draw-only fields into `out` (extraSize bytes).
    void (*capture)(const Actor* actor, uint8_t* out);
    // Checks `in` (extraSize bytes from the network) and, if `actor` is not null, applies it. False (nothing
    // applied) if any field is out of range.
    bool (*apply)(Actor* actor, const uint8_t* in);
    // Whether a host message may create this type with these params (the draw code indexes tables with them).
    bool (*validParams)(uint16_t params);
};

// nullptr if the type has no entry.
const TypeInfo* FindType(int16_t actorId);
// Short name for logs ("EnWf"), or "actor 0x..." text for types without an entry.
const char* TypeName(int16_t actorId);

} // namespace Coop::Enemies

#endif // COOP_ENEMY_TYPES_H
