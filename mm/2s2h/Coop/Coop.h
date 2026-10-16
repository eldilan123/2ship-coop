#ifndef COOP_H
#define COOP_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "CoopNet.h"

// Coop session layer. Everything here runs on the game thread; the network thread lives in CoopNet and only
// exchanges data with the game through queues that are drained once per frame (OnGameStateMainStart).
namespace Coop {

using JsonHandler = std::function<void(uint32_t from, const nlohmann::json& message)>;
using BinaryHandler = std::function<void(uint32_t from, const std::vector<uint8_t>& payload)>;

// Build identifier exchanged in the handshake (git commit hash, unless overridden for testing).
std::string BuildVersion();

bool StartHost(uint16_t port, const std::string& name, Protocol::Color color);
bool StartJoin(const std::string& address, uint16_t port, const std::string& name, Protocol::Color color);
// Leaves or closes the current session. Blocks until the network thread has finished.
void Stop();
// Called once from DeinitOTR so sockets and the thread are closed before the GUI and logger go away.
void Shutdown();

bool IsRunning();
SessionSnapshot GetSnapshot();

// Application messages. `target` is a player id or kBroadcast; clients always send to the host.
// JSON messages need a "type" that is not one of the reserved control types.
void SendJson(uint32_t target, const nlohmann::json& message);
void SendBinary(uint32_t target, uint8_t channel, const void* data, size_t size);
// Handlers run on the game thread, from the per-frame hook.
void RegisterJsonHandler(const std::string& type, JsonHandler handler);
void RegisterBinaryHandler(uint8_t channel, BinaryHandler handler);

} // namespace Coop

#endif // COOP_H
