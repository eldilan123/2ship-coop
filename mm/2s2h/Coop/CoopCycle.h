#ifndef COOP_CYCLE_H
#define COOP_CYCLE_H

#include <cstdint>
#include <string>
#include <vector>

// The three-day cycle (milestone 8). The clock and the cycle are the host's. When the client plays the Song of Time
// (or its inverted or double form), its game does not run it: it asks the host (cycle_request), and the host accepts
// or declines from the Coop window within 60 s (cycle_answer). Accepting runs the song on the host as its own "Yes"
// would:
//  - Song of Time: Message_Update's end-of-cycle save (Sram_SaveEndOfCycle and the flash write), then the transition
//    Player starts for OCARINA_MODE_APPLY_SOT. The host's new scene sends the "cycle" snapshot and the client follows.
//  - Inverted: the time speed toggles as the song's "Yes" does; En_Test6 plays its effect.
//  - Double: the clock goes to the next 6:00 or 18:00 as the song's "Yes" does; En_Test6 plays its effect; the clock
//    channel sends the jump.
// On every cycle snapshot the client ends its own cycle (ClientEndOfCycle): what it had pending in the shared inventory
// is dropped (the host also rejects inventory changes made in another cycle, CoopInventory.cpp), its text box closes,
// its timers stop and its own weekEventReg bits clear.
//
// Game thread only.
namespace Coop::Cycle {

enum class Kind : uint8_t { SongOfTime, Inverted, Double };

// Client: its game was about to ask "return to the first day?" (or the inverted / double question).
void ClientRequest(Kind kind);
// Client: a cycle snapshot was applied.
void ClientEndOfCycle();

struct PendingRequest {
    uint32_t playerId;
    std::string name;
    std::string what;
    int secondsLeft;
};
// Host: the requests waiting for an answer (for the Coop window).
std::vector<PendingRequest> HostPending();
// Host: why a request cannot be accepted right now (a text box, a cutscene...), or nullptr.
const char* HostBusyReason();
void HostAnswer(uint32_t playerId, bool accept);

// Called once per frame from the coop frame hook.
void Frame();

} // namespace Coop::Cycle

#endif // COOP_CYCLE_H
