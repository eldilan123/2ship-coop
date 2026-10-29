#ifndef COOP_SAVE_GUARD_H
#define COOP_SAVE_GUARD_H

// Save guard for coop clients. A client plays on an in-memory copy of the host's save, so nothing may reach the
// client's own save files while it is in a session. Every path that writes save data asks Coop_BlocksSave first.

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

// True while this game is a coop client inside a session. Then `reason` is logged as "[Coop] Save blocked" and the
// caller must skip the write.
bool Coop_BlocksSave(const char* reason);

// True while this game is a coop client inside a session (no logging).
bool Coop_IsClientInSession(void);

#ifdef __cplusplus
}

namespace Coop {
// Game thread, every frame: whether this game is connected to a host as a client (in a session or not yet).
void SetConnectedClient(bool connected);
// Save writes let through while connected to a host as a client, after entering its session once (b-save-write;
// must stay 0). The game's own writes at boot, before entering, do not count.
int SaveWritesWhileClient();
} // namespace Coop
#endif

#endif // COOP_SAVE_GUARD_H
