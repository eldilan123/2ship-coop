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
    // Parse problems, logged once the logger exists
    std::vector<std::string> warnings;
};

const LaunchOptions& GetLaunchOptions();

} // namespace Coop

#endif // COOP_ARGS_H
