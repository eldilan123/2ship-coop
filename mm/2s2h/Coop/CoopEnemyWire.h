#ifndef COOP_ENEMY_WIRE_H
#define COOP_ENEMY_WIRE_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
#include "z64math.h"
}

// Messages of binary channel kChannelEnemies. Every message starts with a Header; the rest depends on its type.
//   Manifest (host -> clients): ManifestHeader + count x SpawnRecord. Every enemy the host is simulating; it replaces
//     whatever the client knew. Sent when the host's scene or rooms change and when a client asks for it.
//   Spawn / Kill (host -> clients): one enemy appears or goes away, tagged with the manifest it applies to.
//   States (host -> clients): StatesHeader + count variable-length state records (see below).
//   Summary (host -> clients): count and hash of the live netIds, every 2 s; a client that disagrees asks again.
//   RequestManifest (client -> host).
// All of it is checked as untrusted input on arrival; a bad message is dropped whole, never partly applied.
namespace Coop::Enemies::Wire {

constexpr uint32_t kMagic = 0x594D4E45; // "ENMY"
constexpr uint8_t kFormatVersion = 2;

enum class Type : uint8_t {
    Manifest = 1,
    Spawn = 2,
    Kill = 3,
    States = 4,
    Summary = 5,
    RequestManifest = 6,
};

enum class KillReason : uint8_t {
    Despawn = 0, // Unloaded by the engine (room change, half day, scene end)
    Died = 1,    // Killed itself in its own update (defeated) or by a test
};

constexpr size_t kMaxRecords = 1024;
constexpr size_t kMaxSkeletons = 4;
constexpr size_t kMaxLimbs = 64; // One bit per joint in a u64 mask
constexpr int kMaxRoom = 31;     // Room clear flags are a u32 bitmask

struct Header {
    uint32_t magic;
    uint8_t formatVersion;
    uint8_t type;
    uint16_t reserved;
};
static_assert(sizeof(Header) == 8);

// One enemy as the host created it. Room actors are matched on the client by (scene, layer, spawnRoom, listIndex,
// actorId), because the client's own copy of the room spawned the same entry. Everything else is created on the
// client from this record alone: actorId, params, home placement and the parent it was spawned from.
struct SpawnRecord {
    uint16_t netId;
    int16_t actorId;
    uint16_t params;   // As given to the spawn (before Init changed anything)
    int16_t listIndex; // Index in the room's actor list, -1 if spawned at run time
    int16_t sceneId;
    int8_t sceneLayer;
    int8_t spawnRoom; // Room whose list spawned it
    int8_t room;      // actor->room now; -1 survives room changes
    uint8_t reserved;
    uint16_t parentNetId; // Enemy it was spawned from (minions, projectiles), 0 if none or not an enemy
    Vec3f homePos;
    Vec3s homeRot;
    int16_t reserved2;
    Vec3f pos;
    Vec3s rot; // shape.rot
    int16_t reserved3;
};
static_assert(sizeof(SpawnRecord) == 56);

struct ManifestHeader {
    uint32_t manifestSeq;
    int16_t sceneId; // -1: the host left its scene, nothing is simulated
    int8_t sceneLayer;
    int8_t curRoom;
    int8_t prevRoom;
    uint8_t reserved;
    uint16_t count;
};
static_assert(sizeof(ManifestHeader) == 12);

struct SpawnMsg {
    uint32_t manifestSeq;
    SpawnRecord record;
};
static_assert(sizeof(SpawnMsg) == 60);

struct KillMsg {
    uint32_t manifestSeq;
    uint16_t netId;
    uint8_t reason;
    uint8_t reserved;
};
static_assert(sizeof(KillMsg) == 8);

struct SummaryMsg {
    uint32_t manifestSeq;
    uint16_t count;
    uint16_t reserved;
    uint32_t hash;
};
static_assert(sizeof(SummaryMsg) == 12);

struct RequestMsg {
    uint32_t reason; // Only for the host's log
};
static_assert(sizeof(RequestMsg) == 4);

struct StatesHeader {
    uint32_t seq;
    uint32_t sentMs;
    uint16_t count;
    uint16_t reserved;
};
static_assert(sizeof(StatesHeader) == 12);

// State record: StateBase, then the optional parts its flags announce, in this order:
//   kScale:      Vec3f scale
//   kLook:       LookPart
//   kActionFunc: int32 offset of actionFunc from a function of this binary (both peers run the same build)
//   skelCount x  { u8 limbCount, u64 changed-joint mask, one Vec3s per set bit }
//   kExtra:      u8 size, then the type's draw-only fields (CoopEnemyTypes)
// kFull marks a keyframe: every part present and every joint sent; the rest are deltas against the previous record
// of the same netId (TCP delivers them all, in order).
struct StateBase {
    uint16_t netId;
    uint8_t flags;
    uint8_t skelCount;
    Vec3f pos;
    Vec3s shapeRot;
    int16_t worldRotY;
};
static_assert(sizeof(StateBase) == 24);

struct LookPart {
    uint16_t colorFilterParams;
    uint8_t colorFilterTimer;
    uint8_t shadowAlpha;
    uint8_t actorFlags; // kLookAttention | kLookHostile
    uint8_t reserved[3];
    Vec3f focusOffset; // focus.pos - world.pos
};
static_assert(sizeof(LookPart) == 20);

constexpr uint8_t kFull = 1 << 0;
constexpr uint8_t kScale = 1 << 1;
constexpr uint8_t kLook = 1 << 2;
constexpr uint8_t kActionFunc = 1 << 3;
constexpr uint8_t kExtra = 1 << 4;
constexpr uint8_t kKnownFlags = kFull | kScale | kLook | kActionFunc | kExtra;

constexpr uint8_t kLookAttention = 1 << 0;
constexpr uint8_t kLookHostile = 1 << 1;

// Bounds-checked reader: any read past the end sets ok = false and returns zeros.
class Reader {
  public:
    Reader(const uint8_t* data, size_t size) : mData(data), mSize(size) {
    }
    template <typename T> T Get() {
        T value{};
        Bytes(&value, sizeof(T));
        return value;
    }
    void Bytes(void* out, size_t size) {
        if (!ok || size > mSize - mPos) {
            ok = false;
            std::memset(out, 0, size);
            return;
        }
        std::memcpy(out, mData + mPos, size);
        mPos += size;
    }
    size_t Remaining() const {
        return mSize - mPos;
    }
    bool ok = true;

  private:
    const uint8_t* mData;
    size_t mSize;
    size_t mPos = 0;
};

class Writer {
  public:
    template <typename T> void Put(const T& value) {
        Bytes(&value, sizeof(T));
    }
    void Bytes(const void* data, size_t size) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        buffer.insert(buffer.end(), p, p + size);
    }
    std::vector<uint8_t> buffer;
};

} // namespace Coop::Enemies::Wire

#endif // COOP_ENEMY_WIRE_H
