#include "CoopSaveGuard.h"
#include "CoopSession.h"

#include <atomic>
#include <spdlog/spdlog.h>
#include "2s2h/GameInteractor/GameInteractor.h"

namespace Coop {

namespace {

std::atomic<bool> sClientInSession = false;

// The VB overrides are (re)registered every time the flag changes, so they run after the port's own handlers for the
// same flags (registered at startup) and win while the client is in a session.
void UpdateSaveVanillaBehaviors() {
    const bool active = sClientInSession.load();
    // Loading or ending a cycle would erase the owl save of the client's own file.
    COND_VB_SHOULD(VB_DELETE_OWL_SAVE, active, { *should = false; });
    // Owl-save timing reads the save back from disk into gSaveContext; with writes blocked that would load the
    // client's own (or an empty) file over the session state.
    COND_VB_SHOULD(VB_SAVE_USE_OWL_SAVE_TIMING, active, { *should = false; });
    // Don't offer the pause-menu save prompt: it could only be blocked.
    COND_VB_SHOULD(VB_SAVE_ON_B_BUTTON_IN_PAUSE_MENU, active, { *should = false; });
}

} // namespace

void SetClientInSession(bool inSession, const char* why) {
    if (sClientInSession.exchange(inSession) == inSession) {
        return;
    }
    SPDLOG_INFO("[Coop] Client session flag {} ({}); saving to disk is {}", inSession ? "ON" : "OFF", why,
                inSession ? "blocked" : "allowed again");
    UpdateSaveVanillaBehaviors();
}

} // namespace Coop

extern "C" bool Coop_IsClientInSession(void) {
    return Coop::sClientInSession.load();
}

extern "C" bool Coop_BlocksSave(const char* reason) {
    if (!Coop::sClientInSession.load()) {
        return false;
    }
    SPDLOG_WARN("[Coop] Save blocked ({}): client in a coop session plays on the host's save", reason);
    return true;
}
