#ifndef COOP_SESSION_H
#define COOP_SESSION_H

#include <cstdint>
#include <string>

// Game-side coop session: the client playing on the host's world. Game thread only.
namespace Coop {

// Turns the "client in a coop session" flag on or off. While it is on, Coop_BlocksSave() blocks every save write.
void SetClientInSession(bool inSession, const char* why);

// Client: asks the host for a snapshot and enters gameplay with it, without choosing a save slot.
void RequestEnterSession();
// Client: true from "Enter session" until the session ends (also while the gamestate switch is in progress).
bool IsClientInSession();
bool IsWaitingToEnter();
// One line for the Coop window describing the client's session state.
std::string SessionStatusText();

// Host: sends a snapshot (Snapshot::Reason) of the current state to every client.
void HostBroadcastSnapshot(uint16_t reason);

// Called once per frame from the coop frame hook.
void SessionFrame();

} // namespace Coop

#endif // COOP_SESSION_H
