#include "Coop.h"

#include <chrono>
#include <memory>
#include <unordered_map>
#include <spdlog/spdlog.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include "CoopArgs.h"
#include "CoopClock.h"
#include "CoopPlayerSync.h"
#include "CoopPuppet.h"
#include "CoopSession.h"
#include "CoopTest.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

// Generated on every build by CoopBuildId.cmake: git commit + hash of uncommitted changes + build config.
extern "C" const char gCoopBuildId[];

namespace Coop {

namespace {

std::unique_ptr<CoopNet> sNet;
std::unordered_map<std::string, JsonHandler> sJsonHandlers;
std::unordered_map<uint8_t, BinaryHandler> sBinaryHandlers;
std::chrono::steady_clock::time_point sInitTime;
bool sQuitRequested = false;

CoopNet& Net() {
    if (sNet == nullptr) {
        sNet = std::make_unique<CoopNet>();
    }
    return *sNet;
}

bool StartSession(SessionConfig config) {
    if (Net().IsRunning()) {
        SPDLOG_WARN("[Coop] A session is already running; stop it first");
        return false;
    }
    config.name = Protocol::SanitizeName(config.name);
    config.buildVersion = BuildVersion();
    config.protocolVersion = ProtocolVersion();
    config.silenceAfterSeconds = GetLaunchOptions().silenceAfterSeconds;
    return Net().Start(config);
}

// Runs once per frame on the game thread: delivers application messages queued by the network thread.
void ProcessInbox() {
    if (sNet == nullptr) {
        return;
    }
    for (auto& message : sNet->TakeInbox()) {
        if (message.frame.kind == Protocol::Frame::Kind::Json) {
            const std::string type = Protocol::GetString(message.frame.json, "type");
            auto it = sJsonHandlers.find(type);
            if (it == sJsonHandlers.end()) {
                SPDLOG_DEBUG("[Coop] No handler for message type '{}' from player {}", type, message.from);
                continue;
            }
            it->second(message.from, message.frame.json);
        } else {
            auto it = sBinaryHandlers.find(message.frame.channel);
            if (it == sBinaryHandlers.end()) {
                SPDLOG_DEBUG("[Coop] No handler for binary channel {} from player {}", message.frame.channel,
                             message.from);
                continue;
            }
            it->second(message.from, message.frame.payload);
        }
    }
}

// Test option --coop-quit-after: close the game through the same path as the Quit menu entry.
void CheckQuitAfter() {
    const int quitAfter = GetLaunchOptions().quitAfterSeconds;
    if (quitAfter <= 0 || sQuitRequested ||
        std::chrono::steady_clock::now() - sInitTime < std::chrono::seconds(quitAfter)) {
        return;
    }
    sQuitRequested = true;
    SPDLOG_INFO("[Coop] TEST: --coop-quit-after {} reached, closing the game", quitAfter);
    Ship::Context::GetRawInstance()->GetWindow()->Close();
}

void OnFrame() {
    ProcessInbox();
    SessionFrame();
    Clock::Frame();
    PlayerSync::Frame();
    Puppet::Frame();
    Test::Frame();
    CheckQuitAfter();
}

void StartFromLaunchOptions() {
    const LaunchOptions& options = GetLaunchOptions();
    for (const auto& warning : options.warnings) {
        SPDLOG_WARN("[Coop] Command line: {}", warning);
    }
    if (!options.fakeVersion.empty()) {
        SPDLOG_WARN("[Coop] TEST: pretending to be build '{}' (--coop-fake-version)", options.fakeVersion);
    }
    if (options.fakeProtocol.has_value()) {
        SPDLOG_WARN("[Coop] TEST: pretending to speak coop protocol {} (--coop-fake-protocol)", *options.fakeProtocol);
    }
    if (options.role == Role::None) {
        return;
    }

    const uint16_t port = options.port.value_or(static_cast<uint16_t>(CVarGetInteger(CVAR_COOP_PORT, kDefaultPort)));
    const std::string name = options.name.value_or(CVarGetString(CVAR_COOP_NAME, "Player"));
    Protocol::Color color;
    if (options.color.has_value()) {
        color = *options.color;
    } else {
        const Color_RGB8 saved = CVarGetColor24(CVAR_COOP_COLOR, { 255, 255, 255 });
        color = { saved.r, saved.g, saved.b };
    }

    if (options.role == Role::Host) {
        SPDLOG_INFO("[Coop] Command line: starting as host on port {}", port);
        StartHost(port, name, color);
    } else {
        SPDLOG_INFO("[Coop] Command line: joining {}:{}", options.address, port);
        StartJoin(options.address, port, name, color);
    }
}

void RegisterCoopHooks() {
    sInitTime = std::chrono::steady_clock::now();
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameStateMainStart>(OnFrame);
    StartFromLaunchOptions();
}

} // namespace

std::string BuildVersion() {
    const std::string& fakeVersion = GetLaunchOptions().fakeVersion;
    return fakeVersion.empty() ? std::string(gCoopBuildId) : fakeVersion;
}

int ProtocolVersion() {
    return GetLaunchOptions().fakeProtocol.value_or(Protocol::kProtocolVersion);
}

bool StartHost(uint16_t port, const std::string& name, Protocol::Color color) {
    SessionConfig config;
    config.role = Role::Host;
    config.port = port;
    config.name = name;
    config.color = color;
    return StartSession(config);
}

bool StartJoin(const std::string& address, uint16_t port, const std::string& name, Protocol::Color color) {
    SessionConfig config;
    config.role = Role::Client;
    config.address = address;
    config.port = port;
    config.name = name;
    config.color = color;
    return StartSession(config);
}

void Stop() {
    if (sNet != nullptr) {
        sNet->Stop();
    }
}

void Shutdown() {
    Stop();
    sNet.reset();
}

bool IsRunning() {
    return sNet != nullptr && sNet->IsRunning();
}

bool IsHosting() {
    return IsRunning() && sNet->GetSnapshot().state == SessionState::Hosting;
}

bool IsConnectedClient() {
    return IsRunning() && sNet->GetSnapshot().state == SessionState::Connected;
}

SessionSnapshot GetSnapshot() {
    if (sNet == nullptr) {
        return {};
    }
    return sNet->GetSnapshot();
}

void SendJson(uint32_t target, const nlohmann::json& message) {
    if (sNet != nullptr) {
        sNet->SendJson(target, message);
    }
}

void SendBinary(uint32_t target, uint8_t channel, const void* data, size_t size) {
    if (sNet != nullptr) {
        sNet->SendBinary(target, channel, data, size);
    }
}

void RegisterJsonHandler(const std::string& type, JsonHandler handler) {
    sJsonHandlers[type] = std::move(handler);
}

void RegisterBinaryHandler(uint8_t channel, BinaryHandler handler) {
    sBinaryHandlers[channel] = std::move(handler);
}

static RegisterShipInitFunc initFunc(RegisterCoopHooks);

} // namespace Coop
