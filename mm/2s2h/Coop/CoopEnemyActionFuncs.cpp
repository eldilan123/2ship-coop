// Where every enemy type keeps its actionFunc, from the generated list (gen_enemy_action_funcs.py). Kept in its own
// file because it includes every enemy overlay header.
#include "CoopEnemyTypes.h"

#include <cstddef>

extern "C" {
#include "z64.h"
#define ENEMY_ACTION_FUNC_INCLUDES
#include "CoopEnemyActionFuncs.inc"
#undef ENEMY_ACTION_FUNC_INCLUDES
}

namespace Coop::Enemies {

namespace {

struct ActionFuncField {
    int16_t actorId;
    ptrdiff_t offset;
};

const ActionFuncField kActionFuncs[] = {
#define ENEMY_ACTION_FUNC(actorId, Struct) { actorId, offsetof(Struct, actionFunc) },
#include "CoopEnemyActionFuncs.inc"
#undef ENEMY_ACTION_FUNC
};

} // namespace

ptrdiff_t ActionFuncOffset(int16_t actorId) {
    for (const ActionFuncField& field : kActionFuncs) {
        if (field.actorId == actorId) {
            return field.offset;
        }
    }
    return -1;
}

} // namespace Coop::Enemies
