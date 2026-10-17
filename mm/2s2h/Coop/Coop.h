#ifndef COOP_H
#define COOP_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "CoopNet.h"

// Coop session layer. Everything here runs on the game thread; the network thread lives in CoopNet and only
// exchanges data with the game through queues that are drained once per frame (OnGameStateMainStart).
#define CVAR_COOP_MODE "gCoop.Mode" // 0 = host, 1 = join
#define CVAR_COOP_ADDRESS "gCoop.Address"
#define CVAR_COOP_PORT "gCoop.Port"
#define CVAR_COOP_NAME "gCoop.Name"
#define CVAR_COOP_COLOR "gCoop.Color"

namespace Coop {

// Removes every --coop-* option from argv, so the ROM extractor never sees them, and remembers them to start a
// session once the game is initialized. Returns the remaining arguments (pointers into argv).
std::vector<char*> ConsumeArgs(int argc, char* argv[]);

using JsonHandler = std::function<void(uint32_t from, const nlohmann::json& message)>;
using BinaryHandler = std::function<void(uint32_t from, const std::vector<uint8_t>& payload)>;

// Build ID exchanged in the handshake: regenerated on every build from the git commit, a hash of uncommitted changes
// and the build config (see CoopBuildId.cmake), unless overridden for testing.
std::string BuildVersion();
// Coop protocol number (Protocol::kProtocolVersion, bumped by hand), unless overridden for testing.
int ProtocolVersion();

bool StartHost(uint16_t port, const std::string& name, Protocol::Color color);
bool StartJoin(const std::string& address, uint16_t port, const std::string& name, Protocol::Color color);
// Leaves or closes the current session. Blocks until the network thread has finished.
void Stop();
// Called once from DeinitOTR so sockets and the thread are closed before the GUI and logger go away.
void Shutdown();

bool IsRunning();
// True while this game hosts a session / is a client whose handshake was accepted.
bool IsHosting();
bool IsConnectedClient();
SessionSnapshot GetSnapshot();

// Application messages. `target` is a player id or kBroadcast; clients always send to the host.
// JSON messages need a "type" that is not one of the reserved control types.
void SendJson(uint32_t target, const nlohmann::json& message);
void SendBinary(uint32_t target, uint8_t channel, const void* data, size_t size);
// Handlers run on the game thread, from the per-frame hook.
void RegisterJsonHandler(const std::string& type, JsonHandler handler);
void RegisterBinaryHandler(uint8_t channel, BinaryHandler handler);
// A received message failed its checks and was thrown away (counted in the [Coop] net-stats line).
void NoteDroppedMessage();

} // namespace Coop

#endif // COOP_H
