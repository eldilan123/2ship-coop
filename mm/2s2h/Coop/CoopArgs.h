#ifndef COOP_ARGS_H
#define COOP_ARGS_H

#include <optional>
#include <string>
#include <vector>
#include "CoopNet.h"

namespace Coop {

// Options given with --coop-* on the command line. They apply to this run only and are never saved to CVars.
struct LaunchOptions {
    Role role = Role::None;
    std::string address;
    std::optional<uint16_t> port;
    std::optional<std::string> name;
    std::optional<Protocol::Color> color;
    // Test only
    std::string fakeVersion;
    std::optional<int> fakeProtocol;
    int quitAfterSeconds = 0;
    int silenceAfterSeconds = 0;
    bool testEnter = false;           // Client: press "Enter session" automatically once connected
    bool testClockLog = false;        // Both: log a clock sample every second while in gameplay
    bool testHostRefuseExits = false; // Host: refuse every scene_request (tests the client timeout)
    bool testPuppetLog = false;       // Both: log the local Link and every puppet once per second
    // Host: boot straight into gameplay on the game's debug save (in memory) at this entrance
    std::optional<uint16_t> testDebugSaveEntrance;
    // Host: boot to file select and load this slot (1-3) through the same route as choosing it
    std::optional<int> autoloadSlot;
    // Host actions, timed in seconds since the host first reached gameplay
    struct TimedValue {
        int value;
        int atSeconds;
        int64_t atUnixMs = 0; // When set (--coop-test-cross-wall): this wall-clock time instead of atSeconds
    };
    std::vector<TimedValue> testGoto;     // value = entrance
    std::vector<TimedValue> testTimeJump; // value = minutes
    std::optional<int> testResetCycleAt;
    // Client: seconds after entering the session to force every save path
    std::optional<int> testForceSaveAt;
    // Client: seconds after entering the session to simulate answering "Yes" to the Song of Time and an owl statue
    std::optional<int> testBlockSongsAt;
    // Client: walk into an exit to this entrance (hex) at these seconds after entering the session
    std::vector<TimedValue> testClientExit;
    // Both: change the local Link at these seconds after reaching gameplay (forms: PlayerTransformation numbers)
    std::vector<TimedValue> testForm;
    struct TimedPos {
        float x;
        float y;
        float z;
        int atSeconds;
    };
    std::vector<TimedPos> testPos;
    bool testEnemyLog = false;   // Both: log the host's enemies and the client's enemy puppets
    bool testKeepHealth = false; // Both: keep the local Link's health full (enemy tests run unattended)
    // Host: spawn an enemy in front of Link at these seconds after reaching gameplay
    struct TimedSpawn {
        int actorId;
        int params;
        int atSeconds;
    };
    std::vector<TimedSpawn> testSpawn;
    // Host: value = netId, -1 = the newest enemy still alive, -2 = every enemy spawned at run time ("todos"), -3 =
    // every enemy alive in the host's room ("cuarto")
    std::vector<TimedValue> testKill;
    // Both: seconds after reaching gameplay to send malformed enemy channel messages to the other side
    std::optional<int> testFuzzEnemiesAt;
    // Both: from this second on, once a second for a minute, log whether the local Link could take out a bomb (the
    // explosive limit, as Player checks it)
    std::optional<int> testBombLimitFrom;
    // Host: from testSweepAt on, every kSweepStepSeconds remove the previous test enemy and spawn the next one
    struct SweepEntry {
        int actorId;
        int params;
    };
    std::vector<SweepEntry> testSweep;
    int testSweepAt = 0;
    int testNetDelayMs = 0; // Both: simulated latency on what this game sends and receives
    // Both: press buttons on the local Link (in-game input, never the OS): `button` is "b", "a", "z", "r" or
    // "roll" (A held with the stick forward); `count` presses every `intervalMs`, each held `holdMs`; with
    // `approach` > 0 the Link is first placed that far in front of the nearest enemy, facing it.
    struct TimedAttack {
        std::string button;
        int count;
        int intervalMs;
        int holdMs;
        int approach;
        int atSeconds;
    };
    std::vector<TimedAttack> testAttack;
    // Both: fire a projectile from the local Link at the nearest enemy (CoopProjectiles::TestShoot): `button` is the
    // kind (arrow, fire, ice, light, bubble, nut, bomb, chu, fins); holdMs is unused.
    std::vector<TimedAttack> testShoot;
    // Both: put the local Link this far from the nearest enemy, facing it (value = distance; negative: on the enemy's
    // other side, away from where the Link was)
    std::vector<TimedValue> testNear;
    // Host: hit the nearest enemy with these damage flags (`button`, hex), `count` times every `intervalMs` (enemy
    // health phase tests)
    std::vector<TimedAttack> testHit;
    // Both: the local Link takes this much damage through play->damagePlayer (value = health units, 16 per heart)
    std::vector<TimedValue> testHurt;
    // Both: the local Link's health set to this value (0 kills it the way any cause does once health reaches 0)
    std::vector<TimedValue> testHealth;
    // Both: a fairy put in the first bottle slot (in memory), at this second
    std::optional<int> testGiveFairyAt;
    // Both: log every change of the local health and of the bottle slots
    bool testLogHealth = false;
    // Both: log the scene's room count and transition actors (doors, room planes) on every room load
    bool testLogTransitions = false;
    // Both: cross the scene's transition actor number `value` (place the Link in front of it, then walk through or
    // press A for a door; game input only)
    std::vector<TimedValue> testCross;
    // Both: hold the local game in a state for `ms` (room move deferrals): "pause" (START to open and close the pause
    // menu), "text" (a text box), "cutscene" (the Link's wait cutscene action), "knock" (knocked back every 400 ms)
    struct TimedHold {
        std::string kind;
        int ms;
        int atSeconds;
    };
    std::vector<TimedHold> testHold;
    // Both: change this game's shared state with the game's own functions (shared state tests): "item:<id>"
    // (Item_Give), "rupees:<n>" (Rupees_ChangeBy, n may be negative), "week:<hex packed flag>" (SET_WEEKEVENTREG),
    // "switch:<flag>" (Flags_SetSwitch in the current scene), "chest:<hex params>" (an En_Box 150 units along +z of the
    // Link, facing it), "drop:<hex params>" (an En_Item00 150 units along -z), "open:0" (the Link in front of the
    // nearest chest, pressing A), "pick:0" (the Link onto the nearest collectible). "@w<unix ms>" instead of
    // "@<seconds>": at that wall-clock time (two games changing together)
    struct TimedChange {
        std::string kind;
        int value;
        int atSeconds;
        int64_t atUnixMs = 0;
    };
    std::vector<TimedChange> testChange;
    // Host: move the local Link to another room of the scene with the room move (room, position)
    struct TimedRoom {
        int room;
        float x, y, z;
        int atSeconds;
        int64_t atUnixMs = 0; // "@w<unix ms>": at this wall-clock time instead
    };
    std::vector<TimedRoom> testRoom;
    // Host: from these seconds on, the local Link taken through every room of the scene with the room move, one every
    // 8 s, 120 units inside one of its doors (a survey: with --coop-test-log-transitions, each room's cutscenes and
    // enemies)
    std::optional<int> testVisitRoomsAt;
    // Both: first id of this game's projectile counter (tests the wrap past 65535)
    int testProjectileFirstId = 0;
    // Parse problems, logged once the logger exists
    std::vector<std::string> warnings;
};

const LaunchOptions& GetLaunchOptions();

} // namespace Coop

#endif // COOP_ARGS_H
