#ifndef COOP_BOSSES_H
#define COOP_BOSSES_H

// Bosses (milestone 8). Level A: the host simulates every boss, Majora included, and the client sees it as a puppet it
// can hit (Enemies::SyncedBoss); the boss keeps going after the host (Odolwa and Goht: the nearest player, see
// CoopTargeting.h). Majora's battle handler is each game's own (Enemies::LocalOnly). The client never fights a boss of
// its own:
//  - its room's copy is a puppet bound to the host's (it never updates, so it cannot die there), and what a boss spawns
//    on the client is removed, as with every enemy;
//  - a boss reward (a heart container, the remains) is never given by the client's own game: it only arrives with the
//    host's inventory (boss-reward-blocked counts any attempt).
//
// Game thread only.
namespace Coop::Bosses {

// Called once per frame from the coop frame hook.
void Frame();

} // namespace Coop::Bosses

#endif // COOP_BOSSES_H
