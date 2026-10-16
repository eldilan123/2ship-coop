#ifndef COOP_PLAYER_SYNC_H
#define COOP_PLAYER_SYNC_H

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "z64.h"
}

// Player state sync (binary channel kChannelPlayerState). Every player sends the pose of its own Link after its
// update, about 20 times per second; the host applies the clients' states and relays them to the other clients.
// Only the resulting pose and the fields the Player draw code reads are sent: no pointers, no animation ids.
namespace Coop::PlayerSync {

// Bump when State changes.
constexpr uint16_t kFormatVersion = 1;
constexpr uint32_t kMagic = 0x53504C50; // "PLPS"
constexpr size_t kAnimNameSize = 96;

struct State {
    uint32_t magic;
    uint16_t formatVersion;
    uint8_t inGameplay; // 0 while the sender is on the title screen, a gamestate switch, etc.
    uint8_t pad0;
    uint32_t playerId; // Set by the host when relaying; the sender's own id otherwise
    uint32_t seq;
    uint32_t sentMs; // Sender's clock, for jitter and interpolation
    int16_t sceneId;
    int8_t roomNum;
    int8_t sceneLayer;
    // Placement
    Vec3f pos;
    Vec3s shapeRot;
    int16_t pad1;
    float speed;
    Vec3f velocity;
    float shapeYOffset;
    // Form, mask, equipment (per player; never applied to the local SaveContext)
    uint8_t form;
    uint8_t currentMask;
    uint8_t prevMask;
    uint8_t modelGroup;
    int8_t currentShield;
    int8_t currentBoots;
    uint8_t sword; // GET_CUR_EQUIP_VALUE(EQUIP_TYPE_SWORD) of the sender
    uint8_t face;
    uint32_t stateFlags1;
    uint32_t stateFlags2;
    uint32_t stateFlags3;
    // Skeleton pose after the update (before the draw edits the legs)
    PlayerAnimationFrame jointTable;
    uint8_t movementFlags;
    uint8_t pad2;
    float curFrame;
    float upperCurFrame;
    char animName[kAnimNameSize]; // The draw code compares animations by name
    char upperAnimName[kAnimNameSize];
    // Posture
    Vec3s headLimbRot;
    Vec3s upperLimbRot;
    int16_t upperLimbYawSecondary;
    int16_t unk_AAA;
    float unk_AB8;
    float unk_ABC;
    // Form-specific draw routes (Goron roll, Zora swim and barrier, Deku flower and flight)
    float unk_B10[6];
    int16_t unk_B28;
    int16_t unk_B62;
    int16_t unk_B86[2];
    int16_t unk_B8A;
    int16_t unk_B8C;
    int16_t unk_B8E;
    int16_t actionVar2;
    int8_t actionVar1;
    uint8_t pad3;
    uint16_t blastMaskTimer;
    Vec3f unk_AF0[2];
};

// One remote player's interpolated state for this frame.
struct Sample {
    State state;      // Discrete fields from the nearest state, pose interpolated
    bool teleported;  // The remote jumped far: snap instead of sliding
    uint32_t delayMs; // Current interpolation delay
};

// Called once per frame from the coop frame hook.
void Frame();
// Interpolated state of every remote player that has sent one. Game thread.
std::vector<Sample> SampleRemotes();
// Newest state received from `playerId` (not interpolated). False if none yet.
bool LatestState(uint32_t playerId, State& out);
// Forget every remote state (session ended).
void Reset();

} // namespace Coop::PlayerSync

#endif // COOP_PLAYER_SYNC_H
