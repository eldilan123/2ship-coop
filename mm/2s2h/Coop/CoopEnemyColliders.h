#ifndef COOP_ENEMY_COLLIDERS_H
#define COOP_ENEMY_COLLIDERS_H

#include <array>
#include <cstdint>
#include <vector>
#include "CoopEnemyWire.h"

extern "C" {
#include "z64.h"
}

// The colliders an enemy can be hit on (registered for AC), as the host had them, so a client can register the
// same colliders on the enemy's puppet and let its own collision check find the local Link's hits.
// A collider is named by its byte offset inside the actor (both peers run the same build); its geometry travels
// relative to the actor's position, so it follows the puppet wherever the client draws it.
namespace Coop::Enemies {

constexpr size_t kMaxAcColliders = 8;
constexpr size_t kMaxAcElements = 32;
// Element flags that describe how an element takes hits (the HIT / DRAW_HITMARK bits are per frame results).
constexpr uint8_t kAcElemKeepFlags =
    ACELEM_ON | ACELEM_HOOKABLE | ACELEM_NO_AT_INFO | ACELEM_NO_DAMAGE | ACELEM_NO_SWORD_SFX | ACELEM_NO_HITMARK;

struct AcElement {
    uint8_t flags = 0;
    std::array<int16_t, 12> geometry = {}; // Per shape, see kGeometryCount in the .cpp
};

struct AcCollider {
    uint16_t offset = 0;
    uint8_t shape = 0;
    uint8_t acFlags = 0;
    std::vector<AcElement> elements;
};

using AcColliders = std::vector<AcCollider>;

// Host, once per frame before the collision check: which colliders each actor registered for AC last frame.
void CaptureAcRegistrations(PlayState* play);
// Host: writes the actor's registered AC colliders, relative to its world position (a count of 0 if none).
void EncodeAcColliders(const Actor* actor, Wire::Writer& w);
// Structural checks only; the colliders are matched against the puppet when applied.
bool DecodeAcColliders(Wire::Reader& r, AcColliders& out, const char*& why);
// The actor's own collider at `offset`, if it really is one of its colliders of that shape and element count.
Collider* PuppetCollider(Actor* actor, uint16_t offset, uint8_t shape, size_t elementCount);
// Client: places the puppet's colliders where the host had them, relative to `base` (the drawn position), and
// registers them for AC. Colliders that do not match the puppet are skipped and counted in `mismatches`.
// `registered` receives the colliders registered this frame.
void RegisterPuppetColliders(PlayState* play, Actor* actor, const AcColliders& colliders, const Vec3f& base,
                             std::vector<Collider*>& registered, int& mismatches);

} // namespace Coop::Enemies

#endif // COOP_ENEMY_COLLIDERS_H
