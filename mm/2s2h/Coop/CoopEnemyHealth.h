#ifndef COOP_ENEMY_HEALTH_H
#define COOP_ENEMY_HEALTH_H

// Host: enemy health scaled by the number of players (see CoopEnemyHealth.cpp). Game thread only.
#define CVAR_COOP_ENEMY_HEALTH_MULT "gCoop.EnemyHealthMult"

namespace Coop::EnemyHealth {

// Called once per frame from the coop frame hook.
void Frame();

} // namespace Coop::EnemyHealth

#endif // COOP_ENEMY_HEALTH_H
