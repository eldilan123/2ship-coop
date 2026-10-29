#ifndef COOP_CUTSCENE_H
#define COOP_CUTSCENE_H

// Cutscenes (milestone 7, phase 3).
// Level 1, always on:
//  - "<player> esta en una cinematica" when another player's Link enters a cutscene (PlayerSync busy flags), and the
//    host's enemies leave a player in a cutscene or a dialogue alone (Targeting).
//  - "<player> consiguio <item>" when another player gets an item that matters (not rupees, hearts, ammo, magic jars,
//    tokens or stray fairies): JSON "got_item", relayed by the host.
// Level 2, the mirror (gCoop.MirrorCutscenes, 0 by default: untested, it may give an item twice): when the client
//  starts a cutscene from an allowlisted actor (the owl, the Great Fairy and the stray fairies, the Song of Healing
//  ones, a big chest), it tells the host ("mirror_cs"); the host, in the same scene and free, plays the same cutscene
//  with its own copy of that actor while its Link waits, and ends it when the client's ends ("mirror_cs_end"), at
//  the latest after kMirrorMaxMs. cutscene-mirror lines log every start, end and skip.
//
// Game thread only.
#define CVAR_COOP_MIRROR_CUTSCENES "gCoop.MirrorCutscenes" // 1: the host mirrors the client's allowlisted cutscenes

namespace Coop::Cutscene {

// Called once per frame from the coop frame hook.
void Frame();

} // namespace Coop::Cutscene

#endif // COOP_CUTSCENE_H
