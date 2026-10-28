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
// Phase 2: whoever crosses first leads. A client that goes through a door, a room plane or a hole reports it
// (room_cross, once the room change is over) and the host follows it with the same light move, next to it; the host
// answers with room_cross_done. If both crossed to different rooms within a second, the host wins and the client is
// taken to the host's room. While the host reels from a hit or falls into the void the follow waits, up to
// gCoop.RoomFollowMaxDeferMs. A client's fall into the void is not a cross: it comes back next to the host. Nobody is
// put down inside a room plane's range (the plane would load a room twice): the arrival spot moves out of it.
//
// The live flags travel only with a move: changes the host makes to the world in between (switches, chests it opens)
// still do not reach the client during play.
//
// Game thread only.
#define CVAR_COOP_ROOM_MOVE_MIN_MS "gCoop.RoomMoveMinMs" // Minimum time between two moves of a player (2000)
// Longest wait of a follow while the host reels from a hit or falls into the void (10000); then it follows anyway.
#define CVAR_COOP_ROOM_FOLLOW_MAX_DEFER_MS "gCoop.RoomFollowMaxDeferMs"

namespace Coop::Rooms {

// Called once per frame from the coop frame hook, before the puppets.
void Frame();
// Whether `room` is loaded in this game (-1, "no room", always is).
bool RoomLoadedHere(int8_t room);
// A move of the local Link is running (fade, rooms being swapped): a client's order or the host's follow.
bool MoveInProgress();

// Test only: sends malformed room messages to the other side.
void TestFuzz();
// Test only (host): moves the local Link to `room` at `pos` with the room move itself (to reach rooms far from a
// scene's entrance); the clients follow as after any room change of the host.
void TestMoveTo(int8_t room, const Vec3f& pos);

} // namespace Coop::Rooms

#endif // COOP_ROOMS_H
