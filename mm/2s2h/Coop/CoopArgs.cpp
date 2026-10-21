#include "CoopArgs.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <fmt/format.h>
#include "Coop.h"

namespace Coop {

namespace {

LaunchOptions sLaunchOptions;

bool ParseInt(const char* text, long min, long max, long& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < min || value > max) {
        return false;
    }
    out = value;
    return true;
}

bool ParseColor(const char* text, Protocol::Color& out) {
    if (text == nullptr) {
        return false;
    }
    if (*text == '#') {
        text++;
    }
    if (std::strlen(text) != 6) {
        return false;
    }
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 16);
    if (*end != '\0') {
        return false;
    }
    out = { static_cast<uint8_t>((value >> 16) & 0xFF), static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>(value & 0xFF) };
    return true;
}

bool ParseHex16(const char* text, uint16_t& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(text, &end, 16);
    if (errno != 0 || *end != '\0' || end == text || value > 0xFFFF) {
        return false;
    }
    out = static_cast<uint16_t>(value);
    return true;
}

// "<value>@<seconds>" where value is hex (entrances) or decimal.
bool ParseTimed(const char* text, bool hexValue, LaunchOptions::TimedValue& out) {
    if (text == nullptr) {
        return false;
    }
    const char* at = std::strchr(text, '@');
    if (at == nullptr) {
        return false;
    }
    const std::string value(text, at - text);
    long seconds = 0;
    if (!ParseInt(at + 1, 0, 24 * 60 * 60, seconds)) {
        return false;
    }
    if (hexValue) {
        uint16_t entrance = 0;
        if (!ParseHex16(value.c_str(), entrance)) {
            return false;
        }
        out.value = entrance;
    } else {
        long number = 0;
        if (!ParseInt(value.c_str(), -100000, 100000, number)) {
            return false;
        }
        out.value = static_cast<int>(number);
    }
    out.atSeconds = static_cast<int>(seconds);
    return true;
}

// "@<seconds>" or "<seconds>"
bool ParseSeconds(const char* text, int& out) {
    if (text == nullptr) {
        return false;
    }
    long seconds = 0;
    if (!ParseInt(text[0] == '@' ? text + 1 : text, 0, 24 * 60 * 60, seconds)) {
        return false;
    }
    out = static_cast<int>(seconds);
    return true;
}

// "<form>@<seconds>"; form is a name or its PlayerTransformation number.
bool ParseForm(const char* text, LaunchOptions::TimedValue& out) {
    static const std::pair<const char*, int> kNames[] = {
        { "fd", 0 },   { "fiercedeity", 0 }, { "deidad", 0 }, { "goron", 1 },
        { "zora", 2 }, { "deku", 3 },        { "human", 4 },  { "humano", 4 },
    };
    const char* at = text != nullptr ? std::strchr(text, '@') : nullptr;
    if (at == nullptr) {
        return false;
    }
    const std::string name(text, at - text);
    long seconds = 0;
    if (!ParseInt(at + 1, 0, 24 * 60 * 60, seconds)) {
        return false;
    }
    long form = -1;
    for (const auto& [key, value] : kNames) {
        if (name == key) {
            form = value;
        }
    }
    if (form < 0 && !ParseInt(name.c_str(), 0, 4, form)) {
        return false;
    }
    out = { static_cast<int>(form), static_cast<int>(seconds) };
    return true;
}

// "<x>,<y>,<z>@<seconds>"
bool ParsePos(const char* text, LaunchOptions::TimedPos& out) {
    if (text == nullptr) {
        return false;
    }
    float x = 0, y = 0, z = 0;
    int seconds = 0;
    char extra = 0;
    if (std::sscanf(text, "%f,%f,%f@%d%c", &x, &y, &z, &seconds, &extra) != 4 || seconds < 0) {
        return false;
    }
    out = { x, y, z, seconds };
    return true;
}

// Decimal, or hex with 0x.
bool ParseNumber(const std::string& text, long min, long max, long& out) {
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        char* end = nullptr;
        errno = 0;
        const long value = std::strtol(text.c_str() + 2, &end, 16);
        if (errno != 0 || *end != '\0' || end == text.c_str() + 2 || value < min || value > max) {
            return false;
        }
        out = value;
        return true;
    }
    return ParseInt(text.c_str(), min, max, out);
}

// "<actorId>,<params>@<seconds>"
bool ParseSpawn(const char* text, LaunchOptions::TimedSpawn& out) {
    const char* comma = text != nullptr ? std::strchr(text, ',') : nullptr;
    const char* at = comma != nullptr ? std::strchr(comma, '@') : nullptr;
    if (at == nullptr) {
        return false;
    }
    long id = 0, params = 0, seconds = 0;
    if (!ParseNumber(std::string(text, comma - text), 0, 0x7FFF, id) ||
        !ParseNumber(std::string(comma + 1, at - comma - 1), 0, 0xFFFF, params) ||
        !ParseInt(at + 1, 0, 24 * 60 * 60, seconds)) {
        return false;
    }
    out = { static_cast<int>(id), static_cast<int>(params), static_cast<int>(seconds) };
    return true;
}

// "<actorId>[:<params>],<actorId>[:<params>],...@<seconds>"
bool ParseSweep(const char* text, std::vector<LaunchOptions::SweepEntry>& out, int& seconds) {
    const char* at = text != nullptr ? std::strrchr(text, '@') : nullptr;
    long start = 0;
    if (at == nullptr || !ParseInt(at + 1, 0, 24 * 60 * 60, start)) {
        return false;
    }
    std::vector<LaunchOptions::SweepEntry> entries;
    std::string list(text, at - text);
    size_t pos = 0;
    while (pos <= list.size()) {
        const size_t comma = std::min(list.find(',', pos), list.size());
        const std::string item = list.substr(pos, comma - pos);
        const size_t colon = item.find(':');
        long id = 0, params = 0;
        if (!ParseNumber(item.substr(0, colon), 0, 0x7FFF, id) ||
            (colon != std::string::npos && !ParseNumber(item.substr(colon + 1), 0, 0xFFFF, params))) {
            return false;
        }
        entries.push_back({ static_cast<int>(id), static_cast<int>(params) });
        pos = comma + 1;
    }
    out = std::move(entries);
    seconds = static_cast<int>(start);
    return !out.empty();
}

// "<button>[:<count>[:<intervalMs>[:<holdMs>[:<approach>]]]]@<seconds>"; for --coop-test-shoot the button is the
// projectile kind.
bool ParseAttack(const char* text, LaunchOptions::TimedAttack& out, bool shoot = false) {
    const char* at = text != nullptr ? std::strrchr(text, '@') : nullptr;
    long seconds = 0;
    if (at == nullptr || !ParseInt(at + 1, 0, 24 * 60 * 60, seconds)) {
        return false;
    }
    std::vector<std::string> parts;
    std::string spec(text, at - text);
    size_t pos = 0;
    while (pos <= spec.size()) {
        const size_t colon = std::min(spec.find(':', pos), spec.size());
        parts.push_back(spec.substr(pos, colon - pos));
        pos = colon + 1;
    }
    const std::string& button = parts[0];
    static const char* const kButtons[] = { "b", "a", "z", "r", "roll" };
    static const char* const kShots[] = { "arrow", "fire", "ice", "light", "bubble", "nut", "bomb", "chu", "fins" };
    const bool known = shoot ? std::find(std::begin(kShots), std::end(kShots), button) != std::end(kShots)
                             : std::find(std::begin(kButtons), std::end(kButtons), button) != std::end(kButtons);
    if (!known) {
        return false;
    }
    long values[4] = { 1, 700, 100, 0 };
    const long limits[4] = { 1000, 60000, 60000, 1000 };
    for (size_t i = 1; i < parts.size() && i <= 4; i++) {
        if (!ParseInt(parts[i].c_str(), 0, limits[i - 1], values[i - 1])) {
            return false;
        }
    }
    out = { button,
            static_cast<int>(values[0]),
            static_cast<int>(values[1]),
            static_cast<int>(values[2]),
            static_cast<int>(values[3]),
            static_cast<int>(seconds) };
    return parts.size() <= 5;
}

// "<netId|ultimo|last>@<seconds>"
bool ParseKill(const char* text, LaunchOptions::TimedValue& out) {
    const char* at = text != nullptr ? std::strchr(text, '@') : nullptr;
    if (at == nullptr) {
        return false;
    }
    const std::string which(text, at - text);
    long netId = -1, seconds = 0;
    if (which == "todos" || which == "all") {
        netId = -2;
    } else if (which != "ultimo" && which != "last" && !ParseInt(which.c_str(), 1, 0xFFFF, netId)) {
        return false;
    }
    if (!ParseInt(at + 1, 0, 24 * 60 * 60, seconds)) {
        return false;
    }
    out = { static_cast<int>(netId), static_cast<int>(seconds) };
    return true;
}

} // namespace

const LaunchOptions& GetLaunchOptions() {
    return sLaunchOptions;
}

std::vector<char*> ConsumeArgs(int argc, char* argv[]) {
    std::vector<char*> remaining;
    LaunchOptions& options = sLaunchOptions;
    bool roleConflict = false;

    for (int i = 0; i < argc; i++) {
        const std::string arg = argv[i];
        if (i == 0 || arg.rfind("--coop-", 0) != 0) {
            remaining.push_back(argv[i]);
            continue;
        }

        auto takeValue = [&]() -> const char* {
            if (i + 1 < argc) {
                return argv[++i];
            }
            options.warnings.push_back(fmt::format("{} needs a value", arg));
            return nullptr;
        };
        auto setRole = [&](Role role) {
            if (options.role != Role::None && options.role != role) {
                roleConflict = true;
            }
            options.role = role;
        };

        long number = 0;
        if (arg == "--coop-host") {
            setRole(Role::Host);
        } else if (arg == "--coop-test-enter") {
            options.testEnter = true;
        } else if (arg == "--coop-test-clock-log") {
            options.testClockLog = true;
        } else if (arg == "--coop-test-host-refuse-exits") {
            options.testHostRefuseExits = true;
        } else if (arg == "--coop-test-puppet-log") {
            options.testPuppetLog = true;
        } else if (arg == "--coop-test-enemy-log") {
            options.testEnemyLog = true;
        } else if (arg == "--coop-test-keep-health") {
            options.testKeepHealth = true;
        } else if (arg == "--coop-test-spawn") {
            const char* value = takeValue();
            LaunchOptions::TimedSpawn timed;
            if (value != nullptr && ParseSpawn(value, timed)) {
                options.testSpawn.push_back(timed);
            } else if (value != nullptr) {
                options.warnings.push_back(
                    fmt::format("--coop-test-spawn: '{}' is not <actorId>,<params>@<seconds>", value));
            }
        } else if (arg == "--coop-test-attack") {
            const char* value = takeValue();
            LaunchOptions::TimedAttack timed;
            if (value != nullptr && ParseAttack(value, timed)) {
                options.testAttack.push_back(timed);
            } else if (value != nullptr) {
                options.warnings.push_back(fmt::format(
                    "--coop-test-attack: '{}' is not <b|a|z|r|roll>[:count[:intervalMs[:holdMs[:approach]]]]@<seconds>",
                    value));
            }
        } else if (arg == "--coop-test-shoot") {
            const char* value = takeValue();
            LaunchOptions::TimedAttack timed;
            if (value != nullptr && ParseAttack(value, timed, true)) {
                options.testShoot.push_back(timed);
            } else if (value != nullptr) {
                options.warnings.push_back(
                    fmt::format("--coop-test-shoot: '{}' is not <arrow|fire|ice|light|bubble|nut|bomb|chu|fins>[:count"
                                "[:intervalMs[:unused[:approach]]]]@<seconds>",
                                value));
            }
        } else if (arg == "--coop-test-proj-first-id") {
            const char* value = takeValue();
            if (value != nullptr && ParseInt(value, 1, 0xFFFF, number)) {
                options.testProjectileFirstId = static_cast<int>(number);
            } else if (value != nullptr) {
                options.warnings.push_back(fmt::format("--coop-test-proj-first-id: '{}' is not 1-65535", value));
            }
        } else if (arg == "--coop-test-net-delay") {
            const char* value = takeValue();
            if (value != nullptr && ParseInt(value, 1, 2000, number)) {
                options.testNetDelayMs = static_cast<int>(number);
            } else if (value != nullptr) {
                options.warnings.push_back(fmt::format("--coop-test-net-delay: '{}' is not 1-2000 ms", value));
            }
        } else if (arg == "--coop-test-sweep") {
            const char* value = takeValue();
            if (value != nullptr && !ParseSweep(value, options.testSweep, options.testSweepAt)) {
                options.warnings.push_back(
                    fmt::format("--coop-test-sweep: '{}' is not <actorId>[:<params>],...@<seconds>", value));
            }
        } else if (arg == "--coop-test-kill") {
            const char* value = takeValue();
            LaunchOptions::TimedValue timed;
            if (value != nullptr && ParseKill(value, timed)) {
                options.testKill.push_back(timed);
            } else if (value != nullptr) {
                options.warnings.push_back(
                    fmt::format("--coop-test-kill: '{}' is not <netId|ultimo>@<seconds>", value));
            }
        } else if (arg == "--coop-test-form") {
            const char* value = takeValue();
            LaunchOptions::TimedValue timed;
            if (value != nullptr && ParseForm(value, timed)) {
                options.testForm.push_back(timed);
            } else if (value != nullptr) {
                options.warnings.push_back(fmt::format("--coop-test-form: '{}' is not <form>@<seconds>", value));
            }
        } else if (arg == "--coop-test-pos") {
            const char* value = takeValue();
            LaunchOptions::TimedPos timed;
            if (value != nullptr && ParsePos(value, timed)) {
                options.testPos.push_back(timed);
            } else if (value != nullptr) {
                options.warnings.push_back(fmt::format("--coop-test-pos: '{}' is not <x>,<y>,<z>@<seconds>", value));
            }
        } else if (arg == "--coop-test-debug-save") {
            const char* value = takeValue();
            uint16_t entrance = 0;
            if (value != nullptr && ParseHex16(value, entrance)) {
                options.testDebugSaveEntrance = entrance;
            } else if (value != nullptr) {
                options.warnings.push_back(fmt::format("--coop-test-debug-save: '{}' is not a hex entrance", value));
            }
        } else if (arg == "--coop-autoload-slot") {
            const char* value = takeValue();
            if (value != nullptr && ParseInt(value, 1, 3, number)) {
                options.autoloadSlot = static_cast<int>(number);
            } else if (value != nullptr) {
                options.warnings.push_back(fmt::format("--coop-autoload-slot: '{}' is not a slot (1-3)", value));
            }
        } else if (arg == "--coop-test-goto" || arg == "--coop-test-timejump" || arg == "--coop-test-client-exit") {
            const bool isGoto = arg != "--coop-test-timejump";
            const char* value = takeValue();
            LaunchOptions::TimedValue timed;
            if (value != nullptr && ParseTimed(value, isGoto, timed)) {
                (arg == "--coop-test-goto"       ? options.testGoto
                 : arg == "--coop-test-timejump" ? options.testTimeJump
                                                 : options.testClientExit)
                    .push_back(timed);
            } else if (value != nullptr) {
                options.warnings.push_back(
                    fmt::format("{}: '{}' is not <{}>@<seconds>", arg, value, isGoto ? "hex entrance" : "minutes"));
            }
        } else if (arg == "--coop-test-reset-cycle" || arg == "--coop-test-force-save" ||
                   arg == "--coop-test-block-songs" || arg == "--coop-test-fuzz-enemies") {
            const char* value = takeValue();
            int seconds = 0;
            if (value != nullptr && ParseSeconds(value, seconds)) {
                (arg == "--coop-test-reset-cycle"   ? options.testResetCycleAt
                 : arg == "--coop-test-force-save"  ? options.testForceSaveAt
                 : arg == "--coop-test-block-songs" ? options.testBlockSongsAt
                                                    : options.testFuzzEnemiesAt) = seconds;
            } else if (value != nullptr) {
                options.warnings.push_back(fmt::format("{}: '{}' is not @<seconds>", arg, value));
            }
        } else if (arg == "--coop-join") {
            if (const char* value = takeValue()) {
                setRole(Role::Client);
                options.address = value;
            }
        } else if (arg == "--coop-port") {
            const char* value = takeValue();
            if (value != nullptr && ParseInt(value, 1, 65535, number)) {
                options.port = static_cast<uint16_t>(number);
            } else if (value != nullptr) {
                options.warnings.push_back(fmt::format("--coop-port: '{}' is not a port number (1-65535)", value));
            }
        } else if (arg == "--coop-name") {
            if (const char* value = takeValue()) {
                options.name = Protocol::SanitizeName(value);
            }
        } else if (arg == "--coop-color") {
            const char* value = takeValue();
            Protocol::Color color;
            if (value != nullptr && ParseColor(value, color)) {
                options.color = color;
            } else if (value != nullptr) {
                options.warnings.push_back(fmt::format("--coop-color: '{}' is not an RRGGBB hex color", value));
            }
        } else if (arg == "--coop-fake-version") {
            if (const char* value = takeValue()) {
                options.fakeVersion = value;
            }
        } else if (arg == "--coop-fake-protocol") {
            const char* value = takeValue();
            if (value != nullptr && ParseInt(value, 0, 1000000, number)) {
                options.fakeProtocol = static_cast<int>(number);
            } else if (value != nullptr) {
                options.warnings.push_back(fmt::format("--coop-fake-protocol: '{}' is not a number", value));
            }
        } else if (arg == "--coop-quit-after" || arg == "--coop-silence-after") {
            const char* value = takeValue();
            if (value != nullptr && ParseInt(value, 1, 24 * 60 * 60, number)) {
                (arg == "--coop-quit-after" ? options.quitAfterSeconds : options.silenceAfterSeconds) =
                    static_cast<int>(number);
            } else if (value != nullptr) {
                options.warnings.push_back(fmt::format("{}: '{}' is not a number of seconds", arg, value));
            }
        } else {
            options.warnings.push_back(fmt::format("Unknown option {}", arg));
        }
    }

    if (roleConflict) {
        options.warnings.push_back("--coop-host and --coop-join are mutually exclusive; not starting a session");
        options.role = Role::None;
    }
    return remaining;
}

} // namespace Coop
