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
    int quitAfterSeconds = 0;
    int silenceAfterSeconds = 0;
    // Parse problems, logged once the logger exists
    std::vector<std::string> warnings;
};

const LaunchOptions& GetLaunchOptions();

} // namespace Coop

#endif // COOP_ARGS_H
