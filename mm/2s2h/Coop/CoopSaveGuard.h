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
#endif

#endif // COOP_SAVE_GUARD_H
