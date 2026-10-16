#include "CoopArgs.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
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
        } else if (arg == "--coop-test-goto" || arg == "--coop-test-timejump") {
            const bool isGoto = arg == "--coop-test-goto";
            const char* value = takeValue();
            LaunchOptions::TimedValue timed;
            if (value != nullptr && ParseTimed(value, isGoto, timed)) {
                (isGoto ? options.testGoto : options.testTimeJump).push_back(timed);
            } else if (value != nullptr) {
                options.warnings.push_back(
                    fmt::format("{}: '{}' is not <{}>@<seconds>", arg, value, isGoto ? "hex entrance" : "minutes"));
            }
        } else if (arg == "--coop-test-reset-cycle" || arg == "--coop-test-force-save" ||
                   arg == "--coop-test-block-songs") {
            const char* value = takeValue();
            int seconds = 0;
            if (value != nullptr && ParseSeconds(value, seconds)) {
                (arg == "--coop-test-reset-cycle"  ? options.testResetCycleAt
                 : arg == "--coop-test-force-save" ? options.testForceSaveAt
                                                   : options.testBlockSongsAt) = seconds;
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
