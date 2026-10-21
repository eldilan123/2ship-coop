#ifndef COOP_TARGETING_H
#define COOP_TARGETING_H

#include <cstdint>

extern "C" {
#include "z64.h"
}

// Host: each enemy goes after the nearest player (milestone 5).
//
// The engine works out every actor's distance and direction to "the player" (the local Link) just before its
// update. For an enemy whose target is a remote player, those fields are rewritten towards that player's puppet,
// and for the length of the enemy's own update the player list starts at the puppet: everything the enemy asks
// GET_PLAYER (position, form, mask, sword out, Player_GetMask...) is about the player it is after. The list is put
// back right after the update; three safety nets put it back if that ever fails, and count it (it must stay 0).
//
// Hysteresis: an enemy switches to another player only if that one is at least 150 units nearer and the last switch
// was at least a second ago (at once if its target is no longer valid: dead, in a cutscene, gone, another room).
// Enemy types that take hold of the player (grab, paralyze, steal) or start a cutscene stay on the host.
//
// Game thread only.
namespace Coop::Targeting {

// Called once per frame from the coop frame hook (first thing: nothing may still be viewing a puppet then).
void Frame();
// The player id an enemy is after (the host's id when it is after the host or not handled).
uint32_t TargetOf(const Actor* enemy);
// Host: the enemy's attack hit `playerId`'s puppet this frame; its next update deals with that player (the player
// view goes to it even if its target is someone else), as it would with the one Link it hit. Not for the types that
// stay on the host.
void NoteVictim(const Actor* enemy, uint32_t playerId);
// Whether this enemy type stays on the host (grabs, paralyzes, steals, cutscenes): it must not hit a puppet either,
// its follow-up would act on the host's Link.
bool StaysOnHost(const Actor* enemy);

} // namespace Coop::Targeting

#endif // COOP_TARGETING_H
