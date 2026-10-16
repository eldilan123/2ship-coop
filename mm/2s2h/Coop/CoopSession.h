#ifndef COOP_SESSION_H
#define COOP_SESSION_H

// Game-side coop session: the client playing on the host's world. Game thread only.
namespace Coop {

// Turns the "client in a coop session" flag on or off. While it is on, Coop_BlocksSave() blocks every save write.
void SetClientInSession(bool inSession, const char* why);

} // namespace Coop

#endif // COOP_SESSION_H
