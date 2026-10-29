#ifndef COOP_PAUSE_H
#define COOP_PAUSE_H

// Pause (milestone 7, phase 2). Without coop the pause menu stops the whole game. In a session the host's world is
// everyone's, so while the host is in the menu with a client in the session its actors and its clock keep running
// (gCoop.PauseKeepsWorld; VB_PAUSE_STOPS_WORLD in Play_UpdateMain and z_kankyo.c). What waits for the menu to close:
//  - the host's own Link and the horse it rides (they would read the menu's buttons), with no knockback building up;
//  - the day and night changes (En_Test4), and the clock stops two minutes before 6:00 and 18:00, so the dawn and the
//    night come after the menu, as without coop;
//  - the enemies that take hold of the player (Targeting::StaysOnHost): they only go after the host's Link;
//  - the camera, the sky, the lights and the music (as in any pause).
// A paused player (host or client) is no enemy's target and takes no damage: the host's Link has no colliders while
// it does not update and the direct damage is dropped; the client drops the hits it gets while paused and 500 ms after
// (Story::ClientBusy). Each player keeps sending its last state while paused, flagged (PlayerSync), so the others
// still see its Link where it stood.
//
// Game thread only.
#define CVAR_COOP_PAUSE_KEEPS_WORLD "gCoop.PauseKeepsWorld" // 1 (default): the host's world runs in its pause menu

namespace Coop::Pause {

// Called once per frame from the coop frame hook.
void Frame();
// The host is in the pause menu and its world keeps running (a client in the session, gCoop.PauseKeepsWorld).
bool HostWorldRuns();
// Host: a direct damage (play->damagePlayer) to its paused Link was dropped.
void NoteDroppedDamage();

} // namespace Coop::Pause

#endif // COOP_PAUSE_H
