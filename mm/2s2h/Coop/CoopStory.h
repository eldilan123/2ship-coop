#ifndef COOP_STORY_H
#define COOP_STORY_H

#include <cstdint>

// Story and NPCs (milestone 6, phase 3). The story flags (weekEventReg and the other story fields of the save) travel
// with the shared inventory (CoopInventory.cpp): what the client does in a dialogue, a shop or a cutscene writes the
// host's save, and the other way round. This module holds what is each player's own and what the client may not do:
//  - kLocalBits: weekEventReg bits that are a player's own state (a minigame in progress, being kicked out of a place
//    at closing time). They are never sent nor overwritten. The list comes from an audit: the bits the game clears and
//    that the interface, the Link or the message system read (CoopStory.cpp has each one with its reason).
//  - kBlocked: NPCs that hold state only one game can run (minigames, races, Anju and Kafei, the robbery). The client
//    cannot talk to them yet: the dialogue closes with a notice and story-blocked counts it (milestone 7).
//  - The Song of Time (and the inverted and double ones), the owl's save and flight, and the NPCs that skip time are
//  the
//    host's: on the client they stop with "solo el host" and story-host-only counts it (milestone 8: ask the host).
//  - A cutscene the client starts runs on the client; what it writes reaches the host. cutscene-from-b counts them.
//  - While the client's Link talks or is in a cutscene (and 500 ms after), it takes no hits from the host's enemies.
//
// Game thread only.
#define CVAR_COOP_STORY_FROM_CLIENT "gCoop.StoryFromClient" // 1 (default): the client's story flags reach the host

namespace Coop::Story {

// The weekEventReg bits of byte `index` that stay each player's own.
uint8_t LocalWeekEventBits(int index);
// Whether the client's story changes go to the host (gCoop.StoryFromClient).
bool FromClient();
// Client: the local Link is talking or in a cutscene, or was until 500 ms ago (hits from the host are dropped then).
bool ClientBusy();

} // namespace Coop::Story

#endif // COOP_STORY_H
