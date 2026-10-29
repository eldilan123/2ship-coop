#include "CoopSnapshot.h"

#include <cstring>
#include <fmt/format.h>

extern "C" {
#include "z64.h"
#include "regs.h"
#include "variables.h"
}

namespace Coop::Snapshot {

namespace {

// Fields that belong to each player. The first snapshot sets them; later snapshots leave the client's values alone.
struct PerPlayer {
    s16 health;
    s8 magic;
    u8 playerForm;
    u8 equippedMask;
    ItemEquips equips;
    DpadSaveInfo dpadEquips;
};

PerPlayer CapturePerPlayer() {
    PerPlayer p;
    p.health = gSaveContext.save.saveInfo.playerData.health;
    p.magic = gSaveContext.save.saveInfo.playerData.magic;
    p.playerForm = gSaveContext.save.playerForm;
    p.equippedMask = gSaveContext.save.equippedMask;
    p.equips = gSaveContext.save.saveInfo.equips;
    p.dpadEquips = gSaveContext.save.shipSaveInfo.dpadEquips;
    return p;
}

void RestorePerPlayer(const PerPlayer& p) {
    gSaveContext.save.saveInfo.playerData.health = p.health;
    gSaveContext.save.saveInfo.playerData.magic = p.magic;
    gSaveContext.save.playerForm = p.playerForm;
    gSaveContext.save.equippedMask = p.equippedMask;
    gSaveContext.save.saveInfo.equips = p.equips;
    gSaveContext.save.shipSaveInfo.dpadEquips = p.dpadEquips;
}

void Append(std::vector<uint8_t>& out, const void* data, size_t size) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

} // namespace

const char* ReasonName(uint16_t reason) {
    switch (static_cast<Reason>(reason)) {
        case Reason::Join:
            return "join";
        case Reason::Scene:
            return "scene";
        case Reason::Cycle:
            return "cycle";
        case Reason::TimeJump:
            return "time jump";
        case Reason::Resync:
            return "resync";
    }
    return "unknown";
}

std::vector<uint8_t> Build(Reason reason, uint32_t seq) {
    Header header = {};
    header.magic = kMagic;
    header.formatVersion = kFormatVersion;
    header.reason = static_cast<uint16_t>(reason);
    header.seq = seq;
    header.saveSize = sizeof(gSaveContext.save);
    header.cycleSceneFlagsSize = sizeof(gSaveContext.cycleSceneFlags);
    header.eventInfSize = sizeof(gSaveContext.eventInf);
    header.masksGivenOnMoonSize = sizeof(gSaveContext.masksGivenOnMoon);
    header.pictoPhotoSize = sizeof(gSaveContext.pictoPhotoI5);
    header.entrance = gSaveContext.save.entrance;
    header.sceneLayer = gSaveContext.sceneLayer;
    header.sceneId = gPlayState != nullptr ? gPlayState->sceneId : -1;
    header.roomNum = gPlayState != nullptr ? gPlayState->roomCtx.curRoom.num : -1;
    header.timeSpeed = R_TIME_SPEED;

    // The live flags of the current scene only reach cycleSceneFlags when the scene is left; copy them now so the
    // snapshot carries what the host sees.
    if (gPlayState != nullptr) {
        Play_SaveCycleSceneFlags(gPlayState);
    }

    std::vector<uint8_t> out;
    out.reserve(sizeof(Header) + header.saveSize + header.cycleSceneFlagsSize + header.eventInfSize +
                header.masksGivenOnMoonSize + header.pictoPhotoSize);
    Append(out, &header, sizeof(header));
    Append(out, &gSaveContext.save, header.saveSize);
    Append(out, gSaveContext.cycleSceneFlags, header.cycleSceneFlagsSize);
    Append(out, gSaveContext.eventInf, header.eventInfSize);
    Append(out, gSaveContext.masksGivenOnMoon, header.masksGivenOnMoonSize);
    Append(out, gSaveContext.pictoPhotoI5, header.pictoPhotoSize);
    return out;
}

bool Parse(const std::vector<uint8_t>& bytes, Parsed& out, std::string& error) {
    if (bytes.size() < sizeof(Header)) {
        error = fmt::format("snapshot too short ({} bytes)", bytes.size());
        return false;
    }
    std::memcpy(&out.header, bytes.data(), sizeof(Header));
    const Header& h = out.header;

    if (h.magic != kMagic) {
        error = "not a session snapshot (bad magic)";
        return false;
    }
    if (h.formatVersion != kFormatVersion) {
        error = fmt::format("snapshot format {} but this build reads format {}. Both players must run the same build.",
                            h.formatVersion, kFormatVersion);
        return false;
    }
    if (h.saveSize != sizeof(gSaveContext.save)) {
        error = fmt::format("host's Save is {} bytes but this build's is {}. Both players must run the same build.",
                            h.saveSize, sizeof(gSaveContext.save));
        return false;
    }
    if (h.cycleSceneFlagsSize != sizeof(gSaveContext.cycleSceneFlags) ||
        h.eventInfSize != sizeof(gSaveContext.eventInf) ||
        h.masksGivenOnMoonSize != sizeof(gSaveContext.masksGivenOnMoon) ||
        h.pictoPhotoSize != sizeof(gSaveContext.pictoPhotoI5)) {
        error = "snapshot block sizes do not match this build. Both players must run the same build.";
        return false;
    }
    const size_t expected = sizeof(Header) + static_cast<size_t>(h.saveSize) + h.cycleSceneFlagsSize + h.eventInfSize +
                            h.masksGivenOnMoonSize + h.pictoPhotoSize;
    if (bytes.size() != expected) {
        error = fmt::format("snapshot is {} bytes, expected {}", bytes.size(), expected);
        return false;
    }

    const uint8_t* cursor = bytes.data() + sizeof(Header);
    out.save = cursor;
    cursor += h.saveSize;
    out.cycleSceneFlags = cursor;
    cursor += h.cycleSceneFlagsSize;
    out.eventInf = cursor;
    cursor += h.eventInfSize;
    out.masksGivenOnMoon = cursor;
    cursor += h.masksGivenOnMoonSize;
    out.pictoPhoto = cursor;
    return true;
}

void ApplyShared(const Parsed& snapshot, bool firstTime) {
    const PerPlayer mine = CapturePerPlayer();
    // Once in the session the clock channel owns these: a time jump is applied after its snapshot (with the day
    // change side effects), so the snapshot must not move the clock first.
    const u16 time = gSaveContext.save.time;
    const s32 day = gSaveContext.save.day;
    const s32 eventDayCount = gSaveContext.save.eventDayCount;
    const s32 isNight = gSaveContext.save.isNight;
    const s32 timeSpeedOffset = gSaveContext.save.timeSpeedOffset;

    std::memcpy(&gSaveContext.save, snapshot.save, sizeof(gSaveContext.save));
    std::memcpy(gSaveContext.cycleSceneFlags, snapshot.cycleSceneFlags, sizeof(gSaveContext.cycleSceneFlags));
    std::memcpy(gSaveContext.eventInf, snapshot.eventInf, sizeof(gSaveContext.eventInf));
    std::memcpy(gSaveContext.masksGivenOnMoon, snapshot.masksGivenOnMoon, sizeof(gSaveContext.masksGivenOnMoon));
    std::memcpy(gSaveContext.pictoPhotoI5, snapshot.pictoPhoto, sizeof(gSaveContext.pictoPhotoI5));

    // The client never owns an owl save.
    gSaveContext.save.isOwlSave = false;

    if (!firstTime) {
        RestorePerPlayer(mine);
        gSaveContext.save.time = time;
        gSaveContext.save.day = day;
        gSaveContext.save.eventDayCount = eventDayCount;
        gSaveContext.save.isNight = isNight;
        gSaveContext.save.timeSpeedOffset = timeSpeedOffset;
    }
}

} // namespace Coop::Snapshot
