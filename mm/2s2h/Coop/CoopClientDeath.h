#ifndef COOP_CLIENT_DEATH_H
#define COOP_CLIENT_DEATH_H

// Client: no game over in the host's world; the Link comes back next to the host (see CoopClientDeath.cpp).
// Game thread only.
namespace Coop::ClientDeath {

// Called once per frame from the coop frame hook.
void Frame();
// The Link is down, waiting to come back.
bool IsDown();

} // namespace Coop::ClientDeath

#endif // COOP_CLIENT_DEATH_H
