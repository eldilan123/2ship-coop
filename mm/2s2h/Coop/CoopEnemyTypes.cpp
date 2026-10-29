#include "CoopEnemyTypes.h"

#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <fmt/format.h>

extern "C" {
#include "z64.h"
#include "functions.h"
#include "variables.h"
#include "overlays/actors/ovl_En_Firefly/z_en_firefly.h"
#include "overlays/actors/ovl_En_Slime/z_en_slime.h"
#include "overlays/actors/ovl_En_St/z_en_st.h"
#include "overlays/actors/ovl_En_Wf/z_en_wf.h"
}

namespace Coop::Enemies {

namespace {

// Bounds for the damage effect fields: Actor_DrawDamageEffects only knows these types, and the floats scale effect
// geometry, so anything huge or not finite is refused.
bool ValidDamageEffect(uint8_t type, float alpha, float scale, float steamScale) {
    switch (type) {
        case ACTOR_DRAW_DMGEFF_FIRE:
        case ACTOR_DRAW_DMGEFF_BLUE_FIRE:
        case ACTOR_DRAW_DMGEFF_FROZEN_NO_SFX:
        case ACTOR_DRAW_DMGEFF_FROZEN_SFX:
        case ACTOR_DRAW_DMGEFF_LIGHT_ORBS:
        case ACTOR_DRAW_DMGEFF_BLUE_LIGHT_ORBS:
        case ACTOR_DRAW_DMGEFF_ELECTRIC_SPARKS_SMALL:
        case ACTOR_DRAW_DMGEFF_ELECTRIC_SPARKS_MEDIUM:
        case ACTOR_DRAW_DMGEFF_ELECTRIC_SPARKS_LARGE:
            break;
        default:
            return false;
    }
    for (float f : { alpha, scale, steamScale }) {
        if (!std::isfinite(f) || f < 0.0f || f > 10.0f) {
            return false;
        }
    }
    return true;
}

bool Finite(const Vec3f& v, float limit) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::fabs(v.x) <= limit &&
           std::fabs(v.y) <= limit && std::fabs(v.z) <= limit;
}

bool AnyParams(uint16_t) {
    return true;
}

// MARK: - Wolfos (En_Wf): the draw picks eye textures by eyeIndex, turns the head by unk_29E and hides the whole
// actor while actionFunc is the "under the ground" one.

struct WfExtra {
    uint8_t eyeIndex;
    uint8_t dmgType;
    int16_t headYaw;
    float dmgAlpha;
    float dmgScale;
    float dmgSteamScale;
};
static_assert(sizeof(WfExtra) == 16);

void WfCapture(const Actor* actor, uint8_t* out) {
    const EnWf* wf = reinterpret_cast<const EnWf*>(actor);
    const WfExtra e = { wf->eyeIndex,        wf->drawDmgEffType,  wf->unk_29E,
                        wf->drawDmgEffAlpha, wf->drawDmgEffScale, wf->drawDmgEffFrozenSteamScale };
    std::memcpy(out, &e, sizeof(e));
}

bool WfApply(Actor* actor, const uint8_t* in) {
    WfExtra e;
    std::memcpy(&e, in, sizeof(e));
    if (e.eyeIndex >= 4 || !ValidDamageEffect(e.dmgType, e.dmgAlpha, e.dmgScale, e.dmgSteamScale)) {
        return false;
    }
    if (actor == nullptr) {
        return true;
    }
    EnWf* wf = reinterpret_cast<EnWf*>(actor);
    wf->eyeIndex = e.eyeIndex;
    wf->unk_29E = e.headYaw;
    wf->drawDmgEffType = e.dmgType;
    wf->drawDmgEffAlpha = e.dmgAlpha;
    wf->drawDmgEffScale = e.dmgScale;
    wf->drawDmgEffFrozenSteamScale = e.dmgSteamScale;
    return true;
}

// MARK: - Keese (En_Firefly): type and aura choose colors and the flame/ice aura; isInvisible switches to XLU and
// hides limbs; the death aura uses timer.

struct FireflyExtra {
    uint8_t auraType;
    uint8_t currentType;
    uint8_t isInvisible;
    uint8_t dmgType;
    int16_t timer;
    int16_t pad;
    float dmgAlpha;
    float dmgScale;
    float dmgSteamScale;
};
static_assert(sizeof(FireflyExtra) == 20);

void FireflyCapture(const Actor* actor, uint8_t* out) {
    const EnFirefly* ff = reinterpret_cast<const EnFirefly*>(actor);
    const FireflyExtra e = { ff->auraType,        ff->currentType,     ff->isInvisible,
                             ff->drawDmgEffType,  ff->timer,           0,
                             ff->drawDmgEffAlpha, ff->drawDmgEffScale, ff->drawDmgEffFrozenSteamScale };
    std::memcpy(out, &e, sizeof(e));
}

bool FireflyApply(Actor* actor, const uint8_t* in) {
    FireflyExtra e;
    std::memcpy(&e, in, sizeof(e));
    // auraType: none, fire, ice; currentType: fire (0) .. ice (4)
    if (e.auraType > 2 || e.currentType > 4 || e.isInvisible > 1 ||
        !ValidDamageEffect(e.dmgType, e.dmgAlpha, e.dmgScale, e.dmgSteamScale)) {
        return false;
    }
    if (actor == nullptr) {
        return true;
    }
    EnFirefly* ff = reinterpret_cast<EnFirefly*>(actor);
    ff->auraType = e.auraType;
    ff->currentType = e.currentType;
    ff->isInvisible = e.isInvisible;
    ff->timer = e.timer;
    ff->drawDmgEffType = e.dmgType;
    ff->drawDmgEffAlpha = e.dmgAlpha;
    ff->drawDmgEffScale = e.dmgScale;
    ff->drawDmgEffFrozenSteamScale = e.dmgSteamScale;
    return true;
}

// MARK: - Chuchu (En_Slime): no skeleton; the body is animated with scale and rotation (generic fields) plus the
// wobble while damaged, the revive puddle, the ice block and the eye texture.

struct SlimeExtra {
    uint8_t iceBlockTimer;
    uint8_t eyeTexIndex;
    uint8_t dmgType;
    uint8_t pad;
    int16_t timer;
    int16_t reviveRotY;
    Vec3f wobbleRot;
    Vec3f reviveScale;
    float dmgAlpha;
    float dmgScale;
    float dmgSteamScale;
};
static_assert(sizeof(SlimeExtra) == 44);

void SlimeCapture(const Actor* actor, uint8_t* out) {
    const EnSlime* sl = reinterpret_cast<const EnSlime*>(actor);
    const SlimeExtra e = { sl->iceBlockTimer,
                           sl->eyeTexIndex,
                           sl->drawDmgEffType,
                           0,
                           sl->timer,
                           sl->reviveRotY,
                           sl->wobbleRot,
                           sl->reviveScale,
                           sl->drawDmgEffAlpha,
                           sl->drawDmgEffScale,
                           sl->drawDmgEffFrozenSteamScale };
    std::memcpy(out, &e, sizeof(e));
}

bool SlimeApply(Actor* actor, const uint8_t* in) {
    SlimeExtra e;
    std::memcpy(&e, in, sizeof(e));
    if (e.eyeTexIndex >= 4 || !Finite(e.wobbleRot, 100.0f) || !Finite(e.reviveScale, 10.0f) ||
        !ValidDamageEffect(e.dmgType, e.dmgAlpha, e.dmgScale, e.dmgSteamScale)) {
        return false;
    }
    if (actor == nullptr) {
        return true;
    }
    EnSlime* sl = reinterpret_cast<EnSlime*>(actor);
    sl->iceBlockTimer = e.iceBlockTimer;
    sl->eyeTexIndex = e.eyeTexIndex;
    sl->timer = e.timer;
    sl->reviveRotY = e.reviveRotY;
    sl->wobbleRot = e.wobbleRot;
    sl->reviveScale = e.reviveScale;
    sl->drawDmgEffType = e.dmgType;
    sl->drawDmgEffAlpha = e.dmgAlpha;
    sl->drawDmgEffScale = e.dmgScale;
    sl->drawDmgEffFrozenSteamScale = e.dmgSteamScale;
    return true;
}

// The draw indexes its color tables with the type in the low byte.
bool SlimeParams(uint16_t params) {
    return (params & 0xFF) < EN_SLIME_TYPE_MAX;
}

// MARK: - Large Skulltula (En_St): sets its skeleton up in an action function, once its object has loaded; a
// puppet's update never runs, so the same skeleton is set up here.

bool NoExtra(Actor*, const uint8_t*) {
    return true;
}

void NoCapture(const Actor*, uint8_t*) {
}

void StPrepare(Actor* actor) {
    EnSt* st = reinterpret_cast<EnSt*>(actor);
    if (st->skelAnime.skeleton == nullptr) {
        SkelAnime_Init(gPlayState, &st->skelAnime, (SkeletonHeader*)object_st_Skel_005298, nullptr, st->jointTable,
                       st->morphTable, OBJECT_ST_LIMB_MAX);
    }
}

const TypeInfo kTypes[] = {
    { ACTOR_EN_WF, "EnWf", sizeof(WfExtra), WfCapture, WfApply, AnyParams, nullptr },
    { ACTOR_EN_FIREFLY, "EnFirefly", sizeof(FireflyExtra), FireflyCapture, FireflyApply, AnyParams, nullptr },
    { ACTOR_EN_SLIME, "EnSlime", sizeof(SlimeExtra), SlimeCapture, SlimeApply, SlimeParams, nullptr },
    { ACTOR_EN_ST, "EnSt", 0, NoCapture, NoExtra, AnyParams, StPrepare },
};
static_assert(sizeof(WfExtra) <= kMaxExtraSize && sizeof(FireflyExtra) <= kMaxExtraSize &&
              sizeof(SlimeExtra) <= kMaxExtraSize);

} // namespace

const TypeInfo* FindType(int16_t actorId) {
    for (const TypeInfo& type : kTypes) {
        if (type.actorId == actorId) {
            return &type;
        }
    }
    return nullptr;
}

namespace {

struct ActorNames {
    const char* debug;
    const char* human;
};

const ActorNames kActorNames[] = {
#define DEFINE_ACTOR(_name, _enumValue, _allocType, debugName, humanName) { debugName, humanName },
#define DEFINE_ACTOR_INTERNAL(_name, _enumValue, _allocType, debugName, humanName) { debugName, humanName },
#define DEFINE_ACTOR_UNSET(_enumValue) { nullptr, nullptr },
#include "tables/actor_table.h"
#undef DEFINE_ACTOR
#undef DEFINE_ACTOR_INTERNAL
#undef DEFINE_ACTOR_UNSET
};
static_assert(sizeof(kActorNames) / sizeof(kActorNames[0]) == ACTOR_ID_MAX);

// Profiles that are not ENEMY but switch to it (Actor_ChangeCategory), in Init or later.
const int16_t kBecomeEnemies[] = { ACTOR_EN_WF, ACTOR_EN_SW, ACTOR_EN_RAF, ACTOR_EN_TUBO_TRAP };

// Types whose Init or update reads their parent as a specific actor: only ever created as a child of its puppet.
const int16_t kNeedsParent[] = { ACTOR_EN_MINIDEATH, ACTOR_EN_JSO, ACTOR_EN_PRZ, ACTOR_EN_DRAGON };

// Enemy types a client must not create from a message: the game crashes when they are created away from what they
// expect (the shooting gallery's keeper, which is not an enemy; the Wizrobe's platforms). They are still matched to
// the client's own copy from its room data. See the sweep table in the milestone 3 report.
const int16_t kNotOnClient[] = { ACTOR_EN_SYATEKI_CROW, ACTOR_EN_SYATEKI_WF, ACTOR_EN_SYATEKI_DEKUNUTS,
                                 ACTOR_EN_SYATEKI_OKUTA, ACTOR_EN_WIZ };

bool InList(const int16_t* list, size_t count, int16_t id) {
    for (size_t i = 0; i < count; i++) {
        if (list[i] == id) {
            return true;
        }
    }
    return false;
}

} // namespace

const char* TypeName(int16_t actorId) {
    if (actorId >= 0 && actorId < ACTOR_ID_MAX && kActorNames[actorId].debug != nullptr) {
        return kActorNames[actorId].debug;
    }
    static std::unordered_map<int16_t, std::string> sNames;
    auto it = sNames.find(actorId);
    if (it == sNames.end()) {
        it = sNames.emplace(actorId, fmt::format("actor 0x{:03X}", static_cast<uint16_t>(actorId))).first;
    }
    return it->second.c_str();
}

const char* TypeDescription(int16_t actorId) {
    if (actorId >= 0 && actorId < ACTOR_ID_MAX && kActorNames[actorId].human != nullptr) {
        return kActorNames[actorId].human;
    }
    return "";
}

bool LocalOnly(int16_t actorId) {
    return actorId == ACTOR_EN_SYATEKI_CROW || actorId == ACTOR_EN_SYATEKI_WF || actorId == ACTOR_EN_SYATEKI_DEKUNUTS ||
           actorId == ACTOR_EN_SYATEKI_OKUTA || actorId == ACTOR_EN_DG;
}

bool CanCreateOnClient(int16_t actorId, bool withParent, const char*& why) {
    if (actorId < 0 || actorId >= ACTOR_ID_MAX || gActorOverlayTable[actorId].profile == nullptr) {
        why = "not in the actor table";
        return false;
    }
    const ActorProfile* profile = gActorOverlayTable[actorId].profile;
    const bool enemy = profile->type == ACTORCAT_ENEMY ||
                       InList(kBecomeEnemies, sizeof(kBecomeEnemies) / sizeof(kBecomeEnemies[0]), actorId);
    if (!enemy) {
        why = "not an enemy type";
        return false;
    }
    if (InList(kNotOnClient, sizeof(kNotOnClient) / sizeof(kNotOnClient[0]), actorId)) {
        why = "type not supported on the client";
        return false;
    }
    if (!withParent && InList(kNeedsParent, sizeof(kNeedsParent) / sizeof(kNeedsParent[0]), actorId)) {
        why = "only created as a child of its parent's puppet";
        return false;
    }
    return true;
}

} // namespace Coop::Enemies
