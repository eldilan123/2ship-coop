#ifndef COOP_SNAPSHOT_H
#define COOP_SNAPSHOT_H

#include <cstdint>
#include <string>
#include <vector>

// Session snapshot: the host's shared world state, sent on binary channel kChannelSnapshot when a client enters the
// session, on every host scene change, on a cycle reset and right before a time jump.
//
// Layout: Header, then the raw bytes of `Save`, `cycleSceneFlags`, `eventInf`, `masksGivenOnMoon` and
// `pictoPhotoI5`. Raw structs are safe because the handshake guarantees both sides run the exact same build; the
// header still carries a format version and every size so a mismatch is rejected instead of misread.
namespace Coop::Snapshot {

// Bump when the header or the list of payload blocks changes.
constexpr uint16_t kFormatVersion = 1;
constexpr uint32_t kMagic = 0x4E535043; // "CPSN"

enum class Reason : uint16_t {
    Join = 1,     // Client asked to enter the session
    Scene = 2,    // Host arrived in a new scene
    Cycle = 3,    // Host reset the 3-day cycle
    TimeJump = 4, // Host's clock jumped; the clock message with the jump follows right after
    Resync = 5,   // A client asked for the whole state again (milestone 8); the inventory comes with it
};
const char* ReasonName(uint16_t reason);

struct Header {
    uint32_t magic;
    uint16_t formatVersion;
    uint16_t reason;
    uint32_t seq;
    uint32_t saveSize;
    uint32_t cycleSceneFlagsSize;
    uint32_t eventInfSize;
    uint32_t masksGivenOnMoonSize;
    uint32_t pictoPhotoSize;
    int32_t entrance;   // gSaveContext.save.entrance of the host's current scene
    int32_t sceneLayer; // gSaveContext.sceneLayer of the host
    int16_t sceneId;    // play->sceneId of the host
    int16_t roomNum;    // Room the host is in (logged only; the client spawns at the entrance)
    int16_t timeSpeed;  // Host's R_TIME_SPEED
    uint16_t pad;
};

struct Parsed {
    Header header;
    const uint8_t* save;
    const uint8_t* cycleSceneFlags;
    const uint8_t* eventInf;
    const uint8_t* masksGivenOnMoon;
    const uint8_t* pictoPhoto;
};

// Host: captures the current state. Game thread, in gameplay only.
std::vector<uint8_t> Build(Reason reason, uint32_t seq);

// Client: validates the bytes and points `out` into them. Returns false with a readable `error` on any mismatch.
bool Parse(const std::vector<uint8_t>& bytes, Parsed& out, std::string& error);

// Client: replaces the shared part of gSaveContext with the snapshot. When `firstTime` is false the per-player
// fields (health, magic, form, mask, equipped buttons) keep the client's own values.
void ApplyShared(const Parsed& snapshot, bool firstTime);

} // namespace Coop::Snapshot

#endif // COOP_SNAPSHOT_H
