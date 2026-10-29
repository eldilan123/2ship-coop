#ifndef COOP_PLAYER_SYNC_H
#define COOP_PLAYER_SYNC_H

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "z64.h"
}

// Player state sync (binary channel kChannelPlayerState). Every player sends the pose of its own Link after its
// update, about 20 times per second (and its last state again every frame while it is in the pause menu); the host
// applies the clients' states and relays them to the other clients. Only the resulting pose and the fields the Player
// draw code reads are sent: no pointers, no animation ids.
namespace Coop::PlayerSync {

// Bump when State changes.
constexpr uint16_t kFormatVersion = 4;
constexpr uint32_t kMagic = 0x53504C50; // "PLPS"
constexpr size_t kAnimNameSize = 96;
constexpr uint8_t kBusyCutscene = 1 << 0;
constexpr uint8_t kBusyTalking = 1 << 1;

struct State {
    uint32_t magic;
    uint16_t formatVersion;
    uint8_t inGameplay; // 0 while the sender is on the title screen, a gamestate switch, etc.
    uint8_t paused;     // 1: the sender is in its pause menu (its last state, sent again every frame)
    uint32_t playerId;  // Set by the host when relaying; the sender's own id otherwise
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
    uint8_t busy; // kBusyCutscene / kBusyTalking: what the sender's Link is doing (enemies leave it alone)
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
    // What enemies read of a player besides placement, form, mask and stateFlags (milestone 5)
    int8_t meleeWeaponState;     // The sword is out (enemies that guard or dodge react to it)
    int8_t meleeWeaponAnimation; // Which swing
    int8_t invincibilityTimer;
    uint8_t csAction; // PlayerCsAction; not NONE while the player is in a cutscene
    uint8_t unk_D57;  // A projectile was just fired
    uint8_t pad4;
    uint16_t focusNetId; // Enemy the player has locked on (the host's netId), 0 if none
    int16_t health;      // In sixteenths of a heart (hearts over the name tag, milestone 5)
    int16_t healthCapacity;
};

// One remote player's interpolated state for this frame.
struct Sample {
    State state;      // Discrete fields from the nearest state, pose interpolated
    bool teleported;  // The remote jumped far: snap instead of sliding
    uint32_t delayMs; // Current interpolation delay
    uint32_t ageMs;   // Since the newest state arrived (a player that stopped sending: title screen, loading)
};

// Called once per frame from the coop frame hook.
void Frame();
// Interpolated state of every remote player that has sent one. Game thread.
std::vector<Sample> SampleRemotes();
// Newest state received from `playerId` (not interpolated), and how long ago it arrived. False if none yet.
bool LatestState(uint32_t playerId, State& out, uint32_t* ageMs = nullptr);
// Forget every remote state (session ended).
void Reset();

} // namespace Coop::PlayerSync

#endif // COOP_PLAYER_SYNC_H
