#ifndef COOP_ROOMS_H
#define COOP_ROOMS_H

#include <cstdint>

extern "C" {
#include "z64.h"
}

// Same room always (milestone 5b). Inside one scene each player's game loads only its own room (and the previous
// one while a door or room plane is being crossed), and the host simulates only the rooms it has loaded. When a
// player's room is not loaded on the other side, they are taken back together.
//
// Phase 1: the host decides. When a client's room is not loaded on the host for a moment (300 ms, so crossing a room
// plane back and forth does nothing), the host sends a room_move order (JSON) with its room, its position and the
// live flags of the scene (switches, cleared rooms, chests, collectibles: so doors that open when a room is cleared
// match the host's). The client moves there without reloading the scene: a 0.5 s fade to black, the old rooms
// unloaded as the game does when it respawns a fallen Link in another room, the flags applied, the host's room
// loaded and the Link placed next to the host. If that fails, the client reloads the scene next to the host (counted
// as a fallback). Moves wait for cutscenes (doors included), text, pause, scene changes and death on either side, and
// at least gCoop.RoomMoveMinMs pass between two moves of a player.
//
// The live flags travel only with a move: changes the host makes to the world in between (switches, chests it opens)
// still do not reach the client during play.
//
// Game thread only.
#define CVAR_COOP_ROOM_MOVE_MIN_MS "gCoop.RoomMoveMinMs" // Minimum time between two moves of a player (2000)

namespace Coop::Rooms {

// Called once per frame from the coop frame hook, before the puppets.
void Frame();
// Whether `room` is loaded in this game (-1, "no room", always is).
bool RoomLoadedHere(int8_t room);
// Client: a move is running (fade, rooms being swapped).
bool MoveInProgress();

// Test only: sends malformed room messages to the other side.
void TestFuzz();

} // namespace Coop::Rooms

#endif // COOP_ROOMS_H
