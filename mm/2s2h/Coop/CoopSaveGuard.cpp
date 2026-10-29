#include "CoopSaveGuard.h"
#include "CoopSession.h"

#include <atomic>
#include <spdlog/spdlog.h>
#include "2s2h/GameInteractor/GameInteractor.h"

namespace Coop {

namespace {

std::atomic<bool> sClientInSession = false;
// Saves are written from the game thread and from the save thread: both read these.
std::atomic<bool> sConnectedClient = false;
// Entered the session at least once on this connection: what the game writes at boot, before that, is its own.
std::atomic<bool> sEnteredOnce = false;
std::atomic<int> sWritesWhileClient = 0;
// Test only: blocks the host's own writes while a --coop-test-* action emulates a save point.
const char* sTestSuppression = nullptr;

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
    if (inSession) {
        sEnteredOnce.store(true);
    }
    SPDLOG_INFO("[Coop] Client session flag {} ({}); saving to disk is {}", inSession ? "ON" : "OFF", why,
                inSession ? "blocked" : "allowed again");
    UpdateSaveVanillaBehaviors();
}

void SetTestSaveSuppression(const char* why) {
    sTestSuppression = why;
}

void SetConnectedClient(bool connected) {
    sConnectedClient.store(connected);
    if (!connected) {
        sEnteredOnce.store(false);
    }
}

int SaveWritesWhileClient() {
    return sWritesWhileClient.load();
}

} // namespace Coop

extern "C" bool Coop_IsClientInSession(void) {
    return Coop::sClientInSession.load();
}

extern "C" bool Coop_BlocksSave(const char* reason) {
    if (Coop::sTestSuppression != nullptr) {
        SPDLOG_WARN("[Coop] Save blocked ({}): {}", reason, Coop::sTestSuppression);
        return true;
    }
    if (!Coop::sClientInSession.load()) {
        if (Coop::sConnectedClient.load() && Coop::sEnteredOnce.load()) {
            const int n = ++Coop::sWritesWhileClient;
            SPDLOG_WARN("[Coop] b-save-write: {} while connected to a host as a client ({} so far)", reason, n);
        }
        return false;
    }
    SPDLOG_WARN("[Coop] Save blocked ({}): client in a coop session plays on the host's save", reason);
    return true;
}
