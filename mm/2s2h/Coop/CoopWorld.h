#ifndef COOP_WORLD_H
#define COOP_WORLD_H

#include <cstdint>

// World objects (milestone 6, phase 2). The live switch and room-clear flags of the scene travel with the shared
// inventory (CoopInventory.cpp, like chests and collectibles); this module makes the other game's objects react when
// one arrives, and shares the objects that have no flag of their own.
//  - Actors that only look at their switch flag in Init (bombable walls and rocks, webs, sun blocks, barrels, puzzle
//    blocks, single torches, floor/eye/crystal switches, sun switches) are spawned again with the same params when
//    their flag changes from the other game: their Init sees it (a broken wall is gone, a switch is down, a torch
//    lit). The list is data (kInitOnly in CoopWorld.cpp). Actors that read their flag every frame react by themselves.
//  - A locked door (Door_Shutter small key / boss, En_Door locked) loses its lock when its flag arrives, without
//    spending another key: the one that opened it was spent once, from the shared pool.
//  - A pot, crate, rock or bush broken in one game (Actor_Kill while its room is loaded) breaks in the other one too:
//    world_obj with its type, params and home position, relayed by the host. Whatever it drops exists only in the game
//    that broke it (known limit).
//
// Game thread only.
namespace Coop::World {

// A switch flag word of the live scene flags was written from the other game: these bits were set / cleared.
void OnRemoteSwitchFlags(int word, uint32_t set, uint32_t cleared);

// Test only: sends malformed world_obj messages to the other side.
void TestFuzz();

} // namespace Coop::World

#endif // COOP_WORLD_H
