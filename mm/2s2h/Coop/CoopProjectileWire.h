#ifndef COOP_PROJECTILE_WIRE_H
#define COOP_PROJECTILE_WIRE_H

#include <cstddef>
#include <cstdint>

// Messages of binary channel kChannelProjectiles. Every message starts with a Header; the rest depends on its type.
//   Spawn: a projectile of `owner` exists from now on (it may already be flying).
//   States: StatesHeader + count x (StateRecord + the type's extra bytes), about 20 times a second.
//   Kill: the owner's projectile is gone; `hitsSent` hit events (channel 5) were sent for it before this message.
// Projectiles are named by (owner, id): the id is the owner's own u16 counter, a namespace of its own that never
// meets the host's enemy netIds. Each player sends only its own projectiles; the host relays a client's messages to
// the other clients unchanged. All of it is checked as untrusted input on arrival; a bad message is dropped whole.
namespace Coop::Projectiles::Wire {

constexpr uint32_t kMagic = 0x4A4F5250; // "PROJ"
constexpr uint8_t kFormatVersion = 1;

enum class Type : uint8_t {
    Spawn = 1,
    States = 2,
    Kill = 3,
};

constexpr size_t kMaxRecords = 64;

struct Header {
    uint32_t magic;
    uint8_t formatVersion;
    uint8_t type;
    uint8_t owner; // Player id that owns (simulates) the projectiles of this message
    uint8_t reserved;
};
static_assert(sizeof(Header) == 8);

struct SpawnMsg {
    uint16_t id;
    uint16_t parentId; // Projectile of the same owner it is attached to (a magic arrow's effect), 0 if none
    int16_t actorId;
    uint16_t params;
    int16_t sceneId;
    int8_t sceneLayer;
    uint8_t reserved;
    uint32_t sentMs; // Owner's clock
    float pos[3];
    int16_t shapeRot[3];
    int16_t worldRotY;
};
static_assert(sizeof(SpawnMsg) == 36);

struct StatesHeader {
    uint32_t sentMs;
    uint16_t seq;
    uint8_t count;
    uint8_t reserved;
};
static_assert(sizeof(StatesHeader) == 8);

constexpr uint8_t kStateHidden = 1 << 0;   // The owner does not draw it (a bomb that already blew up)
constexpr uint8_t kStateAirborne = 1 << 1; // velocity.y != 0 (some draws test it)
constexpr uint8_t kKnownStateFlags = kStateHidden | kStateAirborne;

struct StateRecord {
    uint16_t id;
    uint8_t flags;
    uint8_t extraSize; // Must be the type's own size
    int16_t pos[3];    // World units, rounded
    int16_t shapeRot[3];
    int16_t worldRotY;
    int16_t speed;  // x16
    uint16_t scale; // scale.x x 10000 (every replicated type is drawn with a uniform scale)
};
static_assert(sizeof(StateRecord) == 22);

enum class KillReason : uint8_t {
    Gone = 0, // Its own update ended it (hit, exploded, timed out)
    Left = 1, // Unloaded: the owner changed scene or left the session
    Count = 2,
};

struct KillMsg {
    uint16_t id;
    uint8_t reason;
    uint8_t reserved;
    uint16_t hitsSent;
    uint16_t reserved2;
    uint32_t sentMs;
};
static_assert(sizeof(KillMsg) == 12);

} // namespace Coop::Projectiles::Wire

#endif // COOP_PROJECTILE_WIRE_H
