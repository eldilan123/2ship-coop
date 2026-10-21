#include "CoopProjectileTypes.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "overlays/actors/ovl_Arrow_Fire/z_arrow_fire.h"
#include "overlays/actors/ovl_Arrow_Ice/z_arrow_ice.h"
#include "overlays/actors/ovl_Arrow_Light/z_arrow_light.h"
#include "overlays/actors/ovl_En_Arrow/z_en_arrow.h"
#include "overlays/actors/ovl_En_Bom/z_en_bom.h"
#include "overlays/actors/ovl_En_Bom_Chu/z_en_bom_chu.h"
#include "overlays/actors/ovl_En_Boom/z_en_boom.h"
#include "overlays/actors/ovl_En_Clear_Tag/z_en_clear_tag.h"
#include "overlays/actors/ovl_En_M_Fire1/z_en_m_fire1.h"
#include "overlays/actors/ovl_En_Nutsball/z_en_nutsball.h"
}

#ifdef _WIN32
#include <windows.h>
#endif

namespace Coop::Projectiles {

namespace {

uintptr_t Anchor() {
    return reinterpret_cast<uintptr_t>(&Actor_Kill);
}

bool InCode(int64_t address) {
    static uintptr_t sBegin = 0, sEnd = 0;
    if (sEnd == 0) {
#ifdef _WIN32
        const HMODULE module = GetModuleHandleW(nullptr);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        const auto* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
        sBegin = reinterpret_cast<uintptr_t>(module);
        sEnd = sBegin + nt->OptionalHeader.SizeOfImage;
#else
        sBegin = Anchor() - 256 * 1024 * 1024;
        sEnd = Anchor() + 256 * 1024 * 1024;
#endif
    }
    return address >= static_cast<int64_t>(sBegin) && address < static_cast<int64_t>(sEnd);
}

bool Finite(float f, float lo, float hi) {
    return std::isfinite(f) && f >= lo && f <= hi;
}

template <typename T> T Read(const uint8_t* in) {
    T value;
    std::memcpy(&value, in, sizeof(T));
    return value;
}

template <typename T> void Write(uint8_t* out, const T& value) {
    std::memcpy(out, &value, sizeof(T));
}

bool Always(const Actor*) {
    return true;
}

uint32_t NoHits(const Actor*) {
    return 0;
}

void NoTick(Actor*) {
}

// The copy stays wherever its owner is, whatever room the local Link is in.
void KeepAcrossRooms(Actor* copy) {
    copy->room = -1;
}

// MARK: - Arrows, Deku bubbles and Deku nuts (En_Arrow)
// The draw compares actionFunc (flight draws the trail and the bubble), reads unk_260/unk_261 (sparkle phase,
// stuck) and the bubble's size.

struct ArrowExtra {
    int32_t actionFunc;
    uint8_t timer;    // unk_260
    uint8_t hitFlags; // unk_261
    uint8_t unk262;
    int8_t bubbleState; // bubble.unk_149
    float bubbleSize;   // bubble.unk_144
};
static_assert(sizeof(ArrowExtra) == 12);

bool ArrowParams(uint16_t params) {
    return params <= ARROW_TYPE_DEKU_NUT;
}

bool ArrowShareable(const Actor* actor) {
    // Nocked arrows and growing bubbles hang from the Link; they are shared once they fly.
    return actor->parent == nullptr;
}

uint32_t ArrowHitFlags(const Actor* actor) {
    const s32 params = actor->params;
    uint32_t flags = Actor_GetArrowDmgFlags(params);
    if (params == ARROW_TYPE_FIRE || params == ARROW_TYPE_ICE) {
        flags |= DMG_NORMAL_ARROW; // Water puts the magic out
    }
    return flags;
}

void ArrowCapture(const Actor* actor, uint8_t* out) {
    const EnArrow* a = reinterpret_cast<const EnArrow*>(actor);
    ArrowExtra e = {};
    e.actionFunc = CodeOffset(reinterpret_cast<const void*>(a->actionFunc));
    e.timer = a->unk_260;
    e.hitFlags = a->unk_261;
    e.unk262 = a->unk_262;
    if (actor->params == ARROW_TYPE_DEKU_BUBBLE) {
        e.bubbleState = a->bubble.unk_149;
        e.bubbleSize = a->bubble.unk_144;
    }
    Write(out, e);
}

bool ArrowApply(Actor* copy, const uint8_t* in) {
    const ArrowExtra e = Read<ArrowExtra>(in);
    void* fn = CodeAddress(e.actionFunc);
    if (fn == nullptr || !Finite(e.bubbleSize, 0.0f, 64.0f)) {
        return false;
    }
    if (copy == nullptr) {
        return true;
    }
    EnArrow* a = reinterpret_cast<EnArrow*>(copy);
    // Only compared by the draw code: the copy's update, the one caller, never runs.
    a->actionFunc = reinterpret_cast<EnArrowActionFunc>(fn);
    a->unk_260 = e.timer;
    a->unk_261 = e.hitFlags;
    a->unk_262 = e.unk262;
    if (copy->params == ARROW_TYPE_DEKU_BUBBLE) {
        a->bubble.unk_149 = e.bubbleState;
        a->bubble.unk_144 = e.bubbleSize;
    }
    return true;
}

// EnArrow_Destroy, without what concerns its owner: Magic_Reset (the local player's magic) and the actor the arrow
// carried (a copy never carries one).
void ArrowCopyDestroy(Actor* thisx, PlayState* play) {
    EnArrow* a = reinterpret_cast<EnArrow*>(thisx);
    if (ARROW_IS_ARROW(thisx->params)) {
        Effect_Destroy(play, a->unk_240);
    }
    Collider_DestroyQuad(play, &a->collider);
    if (thisx->child != nullptr) {
        thisx->child->parent = nullptr; // Its magic effect's copy stops drawing
    }
}

void ArrowPrepare(Actor* copy) {
    EnArrow* a = reinterpret_cast<EnArrow*>(copy);
    a->collider.base.atFlags &= ~AT_ON; // Its draw registers it every frame of the flight
    a->unk_264 = nullptr;
    copy->destroy = ArrowCopyDestroy;
    KeepAcrossRooms(copy);
}

// MARK: - Magic arrow effects (Arrow_Fire / Arrow_Ice / Arrow_Light)
// Drawn around their parent arrow (the arrow's copy here) from radius, height, timer and alpha, plus the screen tint
// of the impact. Their Destroy resets the local player's magic: replaced.

struct MagicExtra {
    float height;
    float fill; // screenFillIntensity / blueingEffectMagnitude
    int16_t radius;
    uint16_t timer;
    uint8_t alpha;
    uint8_t reserved[3];
};
static_assert(sizeof(MagicExtra) == 16);

bool ZeroParams(uint16_t params) {
    return params == 0;
}

bool MagicShareable(const Actor* actor) {
    return actor->parent != nullptr && actor->parent->parent == nullptr; // Its arrow has been released
}

template <typename T> void MagicCapture(const Actor* actor, uint8_t* out) {
    const T* m = reinterpret_cast<const T*>(actor);
    MagicExtra e = {};
    e.radius = m->radius;
    e.timer = m->timer;
    e.alpha = m->alpha;
    e.height = m->height;
    if constexpr (std::is_same_v<T, ArrowIce>) {
        e.fill = m->blueingEffectMagnitude;
    } else {
        e.fill = m->screenFillIntensity;
    }
    Write(out, e);
}

template <typename T> bool MagicApply(Actor* copy, const uint8_t* in) {
    const MagicExtra e = Read<MagicExtra>(in);
    if (e.radius < 0 || e.radius > 400 || e.timer > 255 || !Finite(e.height, 0.0f, 200.0f) ||
        !Finite(e.fill, 0.0f, 2.0f)) {
        return false;
    }
    if (copy == nullptr) {
        return true;
    }
    T* m = reinterpret_cast<T*>(copy);
    m->radius = e.radius;
    m->timer = e.timer;
    m->alpha = e.alpha;
    m->height = e.height;
    if constexpr (std::is_same_v<T, ArrowIce>) {
        m->blueingEffectMagnitude = e.fill;
    } else {
        m->screenFillIntensity = e.fill;
    }
    return true;
}

void FireCopyDestroy(Actor* thisx, PlayState* play) {
    ArrowFire* f = reinterpret_cast<ArrowFire*>(thisx);
    Collider_DestroyQuad(play, &f->collider1);
    Collider_DestroyQuad(play, &f->collider2);
}

void NothingToDestroy(Actor*, PlayState*) {
}

void FirePrepare(Actor* copy) {
    copy->destroy = FireCopyDestroy;
    KeepAcrossRooms(copy);
}

void IceLightPrepare(Actor* copy) {
    copy->destroy = NothingToDestroy;
    KeepAcrossRooms(copy);
}

// MARK: - Bombs (En_Bom)
// The draw reads the flash and the scale; the explosion is the update's work, so the copy plays its visible part
// (blast effect, sound, light, quake) when the owner's bomb goes off. Powder kegs are never shared.

struct BombExtra {
    int16_t timer;
    uint8_t flash;  // unk_1F4, 0-140
    uint8_t params; // BOMB_TYPE_BODY, or BOMB_TYPE_EXPLOSION once it went off
};
static_assert(sizeof(BombExtra) == 4);

constexpr int kBombLightFrames = 10;

bool BombParams(uint16_t params) {
    return params <= BOMB_TYPE_EXPLOSION;
}

bool BombShareable(const Actor* actor) {
    return !reinterpret_cast<const EnBom*>(actor)->isPowderKeg;
}

uint32_t BombHitFlags(const Actor*) {
    return DMG_EXPLOSIVES;
}

void BombCapture(const Actor* actor, uint8_t* out) {
    const EnBom* b = reinterpret_cast<const EnBom*>(actor);
    BombExtra e = {};
    e.timer = b->timer;
    e.flash = static_cast<uint8_t>(std::clamp(b->unk_1F4, 0.0f, 255.0f));
    e.params = static_cast<uint8_t>(actor->params);
    Write(out, e);
}

void BombExplodeVisual(EnBom* b) {
    PlayState* play = gPlayState;
    Actor* actor = &b->actor;
    Actor_Spawn(&play->actorCtx, play, ACTOR_EN_CLEAR_TAG, actor->world.pos.x, actor->world.pos.y, actor->world.pos.z,
                0, 0, 0, CLEAR_TAG_PARAMS(CLEAR_TAG_SMALL_EXPLOSION));
    Actor_RequestQuakeAndRumble(actor, play, 3, 10);
    play->envCtx.adjLightSettings.light1Color[0] = play->envCtx.adjLightSettings.light1Color[1] =
        play->envCtx.adjLightSettings.light1Color[2] = 250;
    play->envCtx.adjLightSettings.ambientColor[0] = play->envCtx.adjLightSettings.ambientColor[1] =
        play->envCtx.adjLightSettings.ambientColor[2] = 250;
    Actor_PlaySfx(actor, NA_SE_IT_BOMB_EXPLOSION);
    actor->params = BOMB_TYPE_EXPLOSION; // Its draw shows nothing from now on
    b->timer = kBombLightFrames;
}

bool BombApply(Actor* copy, const uint8_t* in) {
    const BombExtra e = Read<BombExtra>(in);
    if (e.params > BOMB_TYPE_EXPLOSION || e.timer < 0 || e.timer > 200) {
        return false;
    }
    if (copy == nullptr) {
        return true;
    }
    EnBom* b = reinterpret_cast<EnBom*>(copy);
    if (copy->params == BOMB_TYPE_BODY && e.params == BOMB_TYPE_EXPLOSION) {
        BombExplodeVisual(b);
        return true;
    }
    if (copy->params == BOMB_TYPE_BODY) {
        b->timer = e.timer;
        b->unk_1F4 = e.flash;
    }
    return true;
}

void BombPrepare(Actor* copy) {
    KeepAcrossRooms(copy);
}

void BombTick(Actor* copy) {
    PlayState* play = gPlayState;
    EnBom* b = reinterpret_cast<EnBom*>(copy);
    if (copy->params == BOMB_TYPE_BODY) {
        if (copy->scale.x <= 0.0f) {
            return; // Not out yet
        }
        // The fuse, as EnBom_Update draws it (home.pos is the fuse tip, set by the draw).
        Vec3f zero = { 0.0f, 0.0f, 0.0f };
        if ((play->gameplayFrames % 2) == 0) {
            EffectSsGSpk_SpawnFuse(play, copy, &copy->home.pos, &zero, &zero);
        }
        Actor_PlaySfx(copy, NA_SE_IT_BOMB_IGNIT - SFX_FLAG);
        return;
    }
    // The light of the blast fades as in EnBom_Explode.
    if (b->timer > 0) {
        b->timer--;
        for (int i = 0; i < 3; i++) {
            s16& light = play->envCtx.adjLightSettings.light1Color[i];
            light = light > 25 ? light - 25 : 0;
            s16& ambient = play->envCtx.adjLightSettings.ambientColor[i];
            ambient = ambient > 25 ? ambient - 25 : 0;
        }
    }
}

// MARK: - Bombchus (En_Bom_Chu)
// The draw blinks with the timer and shakes with visualJitter; its blast is an En_Bom of its own.

struct ChuExtra {
    int16_t timer;
    uint8_t isMoving;
    int8_t jitter; // visualJitter, x4
};
static_assert(sizeof(ChuExtra) == 4);

void ChuCapture(const Actor* actor, uint8_t* out) {
    const EnBomChu* c = reinterpret_cast<const EnBomChu*>(actor);
    ChuExtra e = {};
    e.timer = c->timer;
    e.isMoving = c->isMoving ? 1 : 0;
    e.jitter = static_cast<int8_t>(std::clamp(c->visualJitter * 4.0f, -127.0f, 127.0f));
    Write(out, e);
}

bool ChuApply(Actor* copy, const uint8_t* in) {
    const ChuExtra e = Read<ChuExtra>(in);
    if (e.timer < 0 || e.timer > 200 || e.isMoving > 1) {
        return false;
    }
    if (copy == nullptr) {
        return true;
    }
    EnBomChu* c = reinterpret_cast<EnBomChu*>(copy);
    c->timer = e.timer;
    c->isMoving = e.isMoving;
    c->visualJitter = e.jitter / 4.0f;
    return true;
}

void ChuTick(Actor* copy) {
    if (reinterpret_cast<EnBomChu*>(copy)->isMoving) {
        Actor_PlaySfx_Flagged2(copy, NA_SE_IT_BOMBCHU_MOVE - SFX_FLAG);
    }
}

// MARK: - Zora fins (En_Boom)
// The draw turns by world.rot.x/y and spins with unk_1CD; it registers its quad every frame (switched off here).
// Its Destroy hands the other fin back to the local Link: replaced.

struct BoomExtra {
    int16_t worldRotX;
    uint16_t reserved;
};
static_assert(sizeof(BoomExtra) == 4);

bool BoomParams(uint16_t params) {
    return params <= ZORA_BOOMERANG_RIGHT;
}

uint32_t BoomHitFlags(const Actor*) {
    return DMG_ZORA_BOOMERANG;
}

void BoomCapture(const Actor* actor, uint8_t* out) {
    Write(out, BoomExtra{ actor->world.rot.x, 0 });
}

bool BoomApply(Actor* copy, const uint8_t* in) {
    if (copy != nullptr) {
        copy->world.rot.x = Read<BoomExtra>(in).worldRotX;
    }
    return true;
}

void BoomCopyDestroy(Actor* thisx, PlayState* play) {
    EnBoom* b = reinterpret_cast<EnBoom*>(thisx);
    Effect_Destroy(play, b->effectIndex);
    Collider_DestroyQuad(play, &b->collider);
}

void BoomPrepare(Actor* copy) {
    EnBoom* b = reinterpret_cast<EnBoom*>(copy);
    b->collider.base.atFlags &= ~AT_ON;
    b->moveTo = nullptr;
    b->unk_1C8 = nullptr;
    copy->destroy = BoomCopyDestroy;
    KeepAcrossRooms(copy);
}

void BoomTick(Actor* copy) {
    EnBoom* b = reinterpret_cast<EnBoom*>(copy);
    b->unk_1CD += copy->params != ZORA_BOOMERANG_LEFT ? -1 : 1;
    Actor_PlaySfx_Flagged(copy, NA_SE_IT_BOOMERANG_FLY - SFX_FLAG);
}

// MARK: - Deku nut flash (En_M_Fire1)
// Nothing to draw; it exists so a hit by the flash has its attacker on the host.

bool Fire1Params(uint16_t params) {
    return params <= 1;
}

uint32_t Fire1HitFlags(const Actor*) {
    return DMG_DEKU_NUT | DMG_UNK_0x12;
}

// MARK: - Mad Scrub nuts and Octorok rocks (En_Nutsball), from the host's enemies: drawn only.

bool NutsballParams(uint16_t params) {
    return params <= 2;
}

void NutsballTick(Actor* copy) {
    copy->home.rot.z += 0x2AA8;
}

void NoCapture(const Actor*, uint8_t*) {
}

bool NoExtra(Actor*, const uint8_t*) {
    return true;
}

const TypeInfo kTypes[] = {
    { ACTOR_EN_ARROW, sizeof(ArrowExtra), true, false, 900.0f, ELEM_MATERIAL_UNK2, ATELEM_NEAREST | ATELEM_SFX_NONE,
      ArrowParams, ArrowShareable, ArrowHitFlags, ArrowCapture, ArrowApply, ArrowPrepare, NoTick },
    { ACTOR_ARROW_FIRE, sizeof(MagicExtra), true, false, 0.0f, 0, 0, ZeroParams, MagicShareable, NoHits,
      MagicCapture<ArrowFire>, MagicApply<ArrowFire>, FirePrepare, NoTick },
    { ACTOR_ARROW_ICE, sizeof(MagicExtra), true, false, 0.0f, 0, 0, ZeroParams, MagicShareable, NoHits,
      MagicCapture<ArrowIce>, MagicApply<ArrowIce>, IceLightPrepare, NoTick },
    { ACTOR_ARROW_LIGHT, sizeof(MagicExtra), true, false, 0.0f, 0, 0, ZeroParams, MagicShareable, NoHits,
      MagicCapture<ArrowLight>, MagicApply<ArrowLight>, IceLightPrepare, NoTick },
    { ACTOR_EN_BOM, sizeof(BombExtra), true, true, 500.0f, ELEM_MATERIAL_UNK0, ATELEM_SFX_NONE, BombParams,
      BombShareable, BombHitFlags, BombCapture, BombApply, BombPrepare, BombTick },
    { ACTOR_EN_BOM_CHU, sizeof(ChuExtra), true, false, 0.0f, 0, 0, ZeroParams, Always, NoHits, ChuCapture, ChuApply,
      KeepAcrossRooms, ChuTick },
    { ACTOR_EN_BOOM, sizeof(BoomExtra), true, false, 700.0f, ELEM_MATERIAL_UNK2, ATELEM_NEAREST | ATELEM_SFX_NORMAL,
      BoomParams, Always, BoomHitFlags, BoomCapture, BoomApply, BoomPrepare, BoomTick },
    { ACTOR_EN_M_FIRE1, 0, true, false, 400.0f, ELEM_MATERIAL_UNK2, ATELEM_SFX_NONE, Fire1Params, Always, Fire1HitFlags,
      NoCapture, NoExtra, KeepAcrossRooms, NoTick },
    { ACTOR_EN_NUTSBALL, 0, false, true, 0.0f, 0, 0, NutsballParams, Always, NoHits, NoCapture, NoExtra,
      KeepAcrossRooms, NutsballTick },
};

} // namespace

const TypeInfo* FindType(int16_t actorId) {
    for (const TypeInfo& type : kTypes) {
        if (type.actorId == actorId) {
            return &type;
        }
    }
    return nullptr;
}

int32_t CodeOffset(const void* fn) {
    return static_cast<int32_t>(reinterpret_cast<intptr_t>(fn) - static_cast<intptr_t>(Anchor()));
}

void* CodeAddress(int32_t offset) {
    const int64_t address = static_cast<int64_t>(Anchor()) + offset;
    return InCode(address) ? reinterpret_cast<void*>(static_cast<uintptr_t>(address)) : nullptr;
}

} // namespace Coop::Projectiles
