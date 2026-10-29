#ifndef COOP_SESSION_H
#define COOP_SESSION_H

#include <cstdint>
#include <string>

// Game-side coop session: the client playing on the host's world. Game thread only.
namespace Coop {

// Turns the "client in a coop session" flag on or off. While it is on, Coop_BlocksSave() blocks every save write.
void SetClientInSession(bool inSession, const char* why);
// Test only: while `why` is not null every save write is blocked too (also on the host), logging `why`.
void SetTestSaveSuppression(const char* why);

// Client: asks the host for a snapshot and enters gameplay with it, without choosing a save slot.
void RequestEnterSession();
// Client: true from "Enter session" until the session ends (also while the gamestate switch is in progress).
bool IsClientInSession();
bool IsWaitingToEnter();
// Client: true once the session's first scene is loaded (not while switching to it).
bool IsClientPlayingInSession();
// One line for the Coop window describing the client's session state.
std::string SessionStatusText();

// Host: sends a snapshot (Snapshot::Reason) of the current state to every client.
void HostBroadcastSnapshot(uint16_t reason);

// Called once per frame from the coop frame hook.
void SessionFrame();
// Client: an exit to another scene is held, waiting for the host to take it.
bool HoldingExit();
// Client: reload the host's scene and arrive next to the host (after the Link died).
void ClientReturnToHost(const char* why);

// Resync (milestone 8). The client asks the host for the whole shared state again: the host sends a Resync snapshot
// (the save, the cycle flags), the inventory with the live flags and the enemy manifest; the client takes all of it,
// drops what it had pending, and follows the host if it is in another scene. At most once every 10 s per client (the
// host enforces it). The detector asks by itself when a field stays different (CoopShared.cpp).
void RequestResync(const std::string& why, bool force = false);
// Host: sends a client back next to the host ("Bring next to me" in the Coop window).
void BringToHost(uint32_t playerId);

} // namespace Coop

#endif // COOP_SESSION_H
