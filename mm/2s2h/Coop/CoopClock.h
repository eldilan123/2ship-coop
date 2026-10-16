#ifndef COOP_CLOCK_H
#define COOP_CLOCK_H

#include <cstdint>

// Host-driven clock. The host sends its time on binary channel kChannelClock 4 times per second and immediately on a
// jump (preceded by a TimeJump snapshot). A client in a session does not tick its own clock (VB_ADVANCE_TIME): it
// extrapolates from the last host message, for at most kMaxExtrapolationMs, and snaps to every new message.
namespace Coop::Clock {

struct Message {
    uint32_t magic;
    uint32_t seq;
    uint16_t time;
    uint16_t flags;
    int32_t day;
    int32_t eventDayCount;
    int32_t timeSpeedOffset;
    int16_t timeSpeed; // R_TIME_SPEED
    int16_t pad;
    float unitsPerSecond; // How fast the host's clock moved over the last interval (0 while stopped)
};

constexpr uint32_t kMagic = 0x4B4C4350; // "PCLK"
constexpr uint16_t kFlagJump = 1 << 0;
constexpr uint16_t kFlagHostInCutscene = 1 << 1;
constexpr int kSendIntervalMs = 250;
constexpr int kMaxExtrapolationMs = 1500;
// Largest per-frame advance that is still "running" (Sun's Song runs at 400 per frame); more is a jump.
constexpr uint16_t kMaxStepPerFrame = 450;
// Maximum client/host difference considered in sync: 2 in-game minutes.
constexpr uint16_t kSyncThreshold = 0x10000 * 2 / 1440;

// Called once per frame from the coop frame hook (host and client).
void Frame();
// Moves the local clock to `time`/`day` with the same side effects a client applies for a host jump.
void JumpTo(uint16_t time, int32_t day);
// Client: called when the session starts or ends, to drop any old host time.
void Reset();

} // namespace Coop::Clock

#endif // COOP_CLOCK_H
