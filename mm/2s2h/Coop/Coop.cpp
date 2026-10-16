#include "Coop.h"

#include <memory>
#include <unordered_map>
#include <spdlog/spdlog.h>
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "build.h"
}

namespace Coop {

namespace {

std::unique_ptr<CoopNet> sNet;
std::unordered_map<std::string, JsonHandler> sJsonHandlers;
std::unordered_map<uint8_t, BinaryHandler> sBinaryHandlers;

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

void OnFrame() {
    ProcessInbox();
}

void RegisterCoopHooks() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameStateMainStart>(OnFrame);
}

} // namespace

std::string BuildVersion() {
    return gGitCommitHash;
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
