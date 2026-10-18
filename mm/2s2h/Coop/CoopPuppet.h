#ifndef COOP_PUPPET_H
#define COOP_PUPPET_H

// Remote players drawn as real Links ("puppets"). A puppet is a secondary ACTOR_PLAYER spawned through the same
// path the Milk Bar uses for its extra Links (shape.rot.x = form + 1, which skips every SaveContext write), then
// moved to ACTORCAT_NPC so it is never GET_PLAYER. Its Player update never runs: the network pose is applied instead.
#include <cstdint>

extern "C" {
#include "z64.h"
}

namespace Coop::Puppet {

// Called once per frame from the coop frame hook, before the game updates its actors.
void Frame();
// The puppet Link of a remote player in this scene, or null.
Player* PuppetOf(uint32_t playerId);

} // namespace Coop::Puppet

#endif // COOP_PUPPET_H
