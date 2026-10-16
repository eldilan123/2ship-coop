#include "CoopPuppet.h"

#include <chrono>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <spdlog/spdlog.h>
#include "Coop.h"
#include "CoopArgs.h"
#include "CoopPlayerSync.h"
#include "CoopSession.h"
#include "2s2h/BenPort.h"
#include "2s2h/NameTag/NameTag.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "z64malloc.h"
// Defined in BenPort.cpp; its header only declares it for C files.
void ResourceMgr_UnregisterSkeleton(SkelAnime* skelAnime);
}

namespace Coop::Puppet {

namespace {

// Visual-only state: these flags make the draw code do something the puppet must not do (first-person limb hiding,
// aiming reticle, held actors and boomerangs it does not have).
constexpr u32 kStripStateFlags1 =
    PLAYER_STATE1_8 | PLAYER_STATE1_CARRYING_ACTOR | PLAYER_STATE1_100000 | PLAYER_STATE1_ZORA_BOOMERANG_THROWN;

struct Entry {
    Player* actor = nullptr;
    uint8_t form = 0;
    uint8_t modelGroup = 0xFF;
    PlayerSync::Sample sample = {};
    bool haveSample = false;
};

// Puppet per remote player id, and the hooks registered for each puppet actor.
std::unordered_map<uint32_t, Entry> sPuppets;
struct ActorHooks {
    uint32_t playerId;
    HOOK_ID shouldUpdate;
    HOOK_ID beforeDraw;
    HOOK_ID afterDraw;
};
std::unordered_map<Actor*, ActorHooks> sActorHooks;

// Set only while a puppet is being spawned, so Player_Init finds its form object (see OnObjectDependency).
bool sSpawning = false;

std::unordered_set<std::string> sAnimNames;

constexpr const char* kNameTagTag = "coop-puppet";

bool InGameplay() {
    return gPlayState != nullptr && gSaveContext.gameMode == GAMEMODE_NORMAL;
}

bool SessionActive() {
    return IsHosting() || IsClientInSession();
}

// The draw code keeps animation pointers and compares them by name, so they must stay valid: names are interned.
const void* InternAnimation(const char* name) {
    if (name[0] == '\0') {
        return nullptr;
    }
    return sAnimNames.insert(name).first->c_str();
}

// MARK: - Applying the network pose

void ApplyPose(Entry& entry) {
    Player* p = entry.actor;
    Actor* a = &p->actor;
    const PlayerSync::State& s = entry.sample.state;

    a->world.pos = s.pos;
    a->shape.rot = s.shapeRot;
    a->world.rot.y = s.shapeRot.y;
    a->speed = s.speed;
    a->velocity = s.velocity;
    a->shape.yOffset = s.shapeYOffset;
    a->shape.face = s.face;

    if (s.modelGroup != entry.modelGroup) {
        Player_SetModelGroup(p, static_cast<PlayerModelGroup>(s.modelGroup));
        entry.modelGroup = s.modelGroup;
    }
    p->currentShield = s.currentShield;
    p->currentBoots = s.currentBoots;
    p->stateFlags1 = s.stateFlags1 & ~kStripStateFlags1;
    p->stateFlags2 = s.stateFlags2;
    p->stateFlags3 = s.stateFlags3;

    std::memcpy(p->skelAnime.jointTable, &s.jointTable, sizeof(s.jointTable));
    p->skelAnime.animation = const_cast<void*>(InternAnimation(s.animName));
    p->skelAnime.curFrame = s.curFrame;
    p->skelAnime.movementFlags = s.movementFlags;
    p->skelAnimeUpper.animation = const_cast<void*>(InternAnimation(s.upperAnimName));
    p->skelAnimeUpper.curFrame = s.upperCurFrame;

    p->headLimbRot = s.headLimbRot;
    p->upperLimbRot = s.upperLimbRot;
    p->upperLimbYawSecondary = s.upperLimbYawSecondary;
    p->unk_AAA = s.unk_AAA;
    p->unk_AB8 = s.unk_AB8;
    p->unk_ABC = s.unk_ABC;
    std::memcpy(p->unk_B10, s.unk_B10, sizeof(p->unk_B10));
    p->unk_B28 = s.unk_B28;
    p->unk_B62 = s.unk_B62;
    p->unk_B86[0] = s.unk_B86[0];
    p->unk_B86[1] = s.unk_B86[1];
    p->unk_B8A = s.unk_B8A;
    p->unk_B8C = s.unk_B8C;
    p->unk_B8E = s.unk_B8E;
    p->av1.actionVar1 = s.actionVar1;
    p->av2.actionVar2 = s.actionVar2;
    p->blastMaskTimer = s.blastMaskTimer;

    // Mask per player: only the puppet's own fields, never gSaveContext.save.equippedMask.
    p->currentMask = s.currentMask;
    p->prevMask = s.prevMask;
    p->maskId = static_cast<s8>(s.currentMask);
    p->maskObjectLoadState = 0;
    p->unk_AF0[0] = s.unk_AF0[0];
    p->unk_AF0[1] = s.unk_AF0[1];

    // Things the puppet never has.
    p->heldActor = nullptr;
    p->zoraBoomerangActor = nullptr;
    p->getItemDrawIdPlusOne = 0;
    p->meleeWeaponState = 0;

    // The shadow needs the floor under the remote Link.
    Vec3f probe = a->world.pos;
    probe.y += 50.0f;
    s32 bgId = 0;
    a->floorHeight = BgCheck_EntityRaycastFloor5(&gPlayState->colCtx, &a->floorPoly, &bgId, a, &probe);
    a->floorBgId = bgId;
    if (a->floorPoly != nullptr && a->world.pos.y - a->floorHeight < 10.0f) {
        a->bgCheckFlags |= BGCHECKFLAG_GROUND;
    } else {
        a->bgCheckFlags &= ~BGCHECKFLAG_GROUND;
    }
}

// The puppet's Player update never runs, so its body collider is registered here. OC only (bodies push each other);
// AT and AC were switched off at spawn, so it neither deals nor takes damage. Skipped whenever either Link is in a
// cutscene (doors are cutscenes), talking, or a scene change is running, so nobody gets stuck in a doorway.
void RegisterBodyCollision(Entry& entry) {
    Player* p = entry.actor;
    Player* local = GET_PLAYER(gPlayState);
    const u32 busy = PLAYER_STATE1_20000000 | PLAYER_STATE1_TALKING | PLAYER_STATE1_DEAD;
    if ((local->stateFlags1 & busy) || (entry.sample.state.stateFlags1 & busy) ||
        local->csAction != PLAYER_CSACTION_NONE || gPlayState->transitionTrigger != TRANS_TRIGGER_OFF ||
        gPlayState->transitionMode != TRANS_MODE_OFF) {
        return;
    }
    // Same shape Player_UpdateCommon gives the main Link: from the feet to just above the head (bodyPartsPos is
    // written by the puppet's previous draw).
    const f32 feetY =
        (p->bodyPartsPos[PLAYER_BODYPART_LEFT_FOOT].y + p->bodyPartsPos[PLAYER_BODYPART_RIGHT_FOOT].y) * 0.5f;
    const f32 headY = p->bodyPartsPos[PLAYER_BODYPART_HEAD].y + 10.0f;
    p->cylinder.dim.radius = 12;
    p->cylinder.dim.height = static_cast<s16>(headY > feetY ? headY - feetY : 50.0f);
    p->cylinder.dim.yShift = static_cast<s16>(headY > feetY ? feetY - p->actor.world.pos.y : 0.0f);
    // Same mass as the local Link, so a push is shared: the puppet's half is undone by the next network pose and the
    // local Link only gets nudged, never pinned against a wall.
    p->actor.colChkInfo.mass = local->actor.colChkInfo.mass;
    p->actor.colChkInfo.displacement = { 0.0f, 0.0f, 0.0f };
    Collider_UpdateCylinder(&p->actor, &p->cylinder);
    CollisionCheck_SetOC(gPlayState, &gPlayState->colChkCtx, &p->cylinder.base);
}

// Runs inside Actor_UpdateAll for the puppet: apply the network pose instead of Player_Update.
void OnShouldUpdate(Actor* actor, bool* should) {
    *should = false;
    auto hooks = sActorHooks.find(actor);
    if (hooks == sActorHooks.end()) {
        return;
    }
    auto entry = sPuppets.find(hooks->second.playerId);
    if (entry == sPuppets.end() || entry->second.actor != reinterpret_cast<Player*>(actor) ||
        !entry->second.haveSample) {
        return;
    }
    ApplyPose(entry->second);
    RegisterBodyCollision(entry->second);
}

// A few draw routines read the local save instead of the Player being drawn: the sword in hand and in the sheath
// (GET_CUR_EQUIP_VALUE) and the jinx glow (gSaveContext.jinxTimer). Around the puppet's draw they are switched to the
// remote player's values and put back right after, so the local game never sees them changed.
struct DrawSwap {
    bool active = false;
    u16 equipment = 0;
    u16 jinxTimer = 0;
} sDrawSwap;

void RestoreDrawSwap() {
    if (sDrawSwap.active) {
        gSaveContext.save.saveInfo.equips.equipment = sDrawSwap.equipment;
        gSaveContext.jinxTimer = sDrawSwap.jinxTimer;
        sDrawSwap.active = false;
    }
}

void BeforeDraw(Actor* actor, bool* should) {
    if (!*should) {
        return;
    }
    auto hooks = sActorHooks.find(actor);
    if (hooks == sActorHooks.end()) {
        return;
    }
    auto entry = sPuppets.find(hooks->second.playerId);
    if (entry == sPuppets.end() || !entry->second.haveSample) {
        return;
    }
    sDrawSwap = { true, gSaveContext.save.saveInfo.equips.equipment, gSaveContext.jinxTimer };
    SET_EQUIP_VALUE(EQUIP_TYPE_SWORD, entry->second.sample.state.sword);
    gSaveContext.jinxTimer = 0;
}

void AfterDraw(Actor*) {
    RestoreDrawSwap();
}

// MARK: - Lifetime

// Replaces Player_Destroy: frees what the secondary init path created, and nothing global (Player_Destroy would also
// reset the local player's magic and stop fanfares).
void PuppetDestroy(Actor* actor, PlayState* play) {
    Player* p = reinterpret_cast<Player*>(actor);
    ResourceMgr_UnregisterSkeleton(&p->skelAnime);
    ResourceMgr_UnregisterSkeleton(&p->skelAnimeUpper);
    Effect_Destroy(play, p->meleeWeaponEffectIndex[0]);
    Effect_Destroy(play, p->meleeWeaponEffectIndex[1]);
    Effect_Destroy(play, p->meleeWeaponEffectIndex[2]);
    Collider_DestroyCylinder(play, &p->cylinder);
    Collider_DestroyCylinder(play, &p->shieldCylinder);
    Collider_DestroyQuad(play, &p->meleeWeaponQuads[0]);
    Collider_DestroyQuad(play, &p->meleeWeaponQuads[1]);
    Collider_DestroyQuad(play, &p->shieldQuad);
    if (p->maskObjectSegment != nullptr) {
        ZeldaArena_Free(p->maskObjectSegment);
        p->maskObjectSegment = nullptr;
    }

    auto hooks = sActorHooks.find(actor);
    if (hooks != sActorHooks.end()) {
        GameInteractor::Instance->UnregisterGameHookForPtr<GameInteractor::ShouldActorUpdate>(
            hooks->second.shouldUpdate);
        GameInteractor::Instance->UnregisterGameHookForPtr<GameInteractor::ShouldActorDraw>(hooks->second.beforeDraw);
        GameInteractor::Instance->UnregisterGameHookForPtr<GameInteractor::OnActorDraw>(hooks->second.afterDraw);
        auto entry = sPuppets.find(hooks->second.playerId);
        if (entry != sPuppets.end() && entry->second.actor == p) {
            entry->second.actor = nullptr;
        }
        sActorHooks.erase(hooks);
    }
}

void Kill(uint32_t playerId, Entry& entry, const char* why) {
    if (entry.actor != nullptr) {
        SPDLOG_INFO("[Coop] Puppet for player {} removed ({})", playerId, why);
        Actor_Kill(&entry.actor->actor);
        entry.actor = nullptr;
    }
}

// Player_Init resets these shared values on every run; a puppet must leave them as the local player set them.
struct PlayerGlobals {
    s16 profileObjectId;
    void* playerInit;
    void* playerUpdate;
    void* unk_18770;
    void* startPlayerFishing;
    void* grabPlayer;
    void* tryPlayerCsAction;
    void* func_18780;
    void* damagePlayer;
    void* talkWithPlayer;
    void* unk_1878C;
    void* unk_18790;
    void* unk_18794;
    void* setPlayerTalkAnim;
};

PlayerGlobals SavePlayerGlobals() {
    PlayState* play = gPlayState;
    return { gActorOverlayTable[ACTOR_PLAYER].profile->objectId,
             (void*)play->playerInit,
             (void*)play->playerUpdate,
             (void*)play->unk_18770,
             (void*)play->startPlayerFishing,
             (void*)play->grabPlayer,
             (void*)play->tryPlayerCsAction,
             (void*)play->func_18780,
             (void*)play->damagePlayer,
             (void*)play->talkWithPlayer,
             (void*)play->unk_1878C,
             (void*)play->unk_18790,
             (void*)play->unk_18794,
             (void*)play->setPlayerTalkAnim };
}

void RestorePlayerGlobals(const PlayerGlobals& g) {
    PlayState* play = gPlayState;
    const PlayerGlobals now = SavePlayerGlobals();
    if (std::memcmp(&now, &g, sizeof(g)) != 0) {
        SPDLOG_INFO("[Coop] Puppet init changed shared Player values (object id {} -> {}); restored", g.profileObjectId,
                    now.profileObjectId);
    }
    gActorOverlayTable[ACTOR_PLAYER].profile->objectId = g.profileObjectId;
    std::memcpy(&play->playerInit, &g.playerInit, sizeof(void*));
    std::memcpy(&play->playerUpdate, &g.playerUpdate, sizeof(void*));
    std::memcpy(&play->unk_18770, &g.unk_18770, sizeof(void*));
    std::memcpy(&play->startPlayerFishing, &g.startPlayerFishing, sizeof(void*));
    std::memcpy(&play->grabPlayer, &g.grabPlayer, sizeof(void*));
    std::memcpy(&play->tryPlayerCsAction, &g.tryPlayerCsAction, sizeof(void*));
    std::memcpy(&play->func_18780, &g.func_18780, sizeof(void*));
    std::memcpy(&play->damagePlayer, &g.damagePlayer, sizeof(void*));
    std::memcpy(&play->talkWithPlayer, &g.talkWithPlayer, sizeof(void*));
    std::memcpy(&play->unk_1878C, &g.unk_1878C, sizeof(void*));
    std::memcpy(&play->unk_18790, &g.unk_18790, sizeof(void*));
    std::memcpy(&play->unk_18794, &g.unk_18794, sizeof(void*));
    std::memcpy(&play->setPlayerTalkAnim, &g.setPlayerTalkAnim, sizeof(void*));
}

bool Spawn(uint32_t playerId, Entry& entry, const PlayerSync::State& s) {
    PlayState* play = gPlayState;
    Player* local = GET_PLAYER(play);
    const PlayerGlobals globals = SavePlayerGlobals();

    sSpawning = true;
    // Same parameters the Milk Bar uses for its extra Links: rot.x = form + 1 selects the form and skips the
    // SaveContext block of Player_Init.
    Actor* actor = Actor_Spawn(&play->actorCtx, play, ACTOR_PLAYER, s.pos.x, s.pos.y, s.pos.z, s.form + 1, s.shapeRot.y,
                               0, PLAYER_PARAMS(0xFF, PLAYER_START_MODE_F) | 0xFFFFF000);
    sSpawning = false;

    RestorePlayerGlobals(globals);
    // Player_Init loaded the movement tuning (REG values) of the puppet's form; put back the local player's.
    func_80123140(play, local);

    if (actor == nullptr || actor->update == nullptr) {
        SPDLOG_WARN("[Coop] Could not create the puppet for player {} (form {})", playerId, s.form);
        return false;
    }
    Player* p = reinterpret_cast<Player*>(actor);

    // Assets resolve by name in the port, so gameplay_keep's slot (never unloaded) is enough for any form, and the
    // local player's form slot (swapped when it transforms) is never shared.
    actor->objectSlot = play->objectCtx.mainKeepSlot;
    Actor_ChangeCategory(play, &play->actorCtx, actor, ACTORCAT_NPC);
    actor->flags &= ~(ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_FRIENDLY);
    actor->flags |= ACTOR_FLAG_FREEZE_EXCEPTION | ACTOR_FLAG_UPDATE_DURING_OCARINA |
                    ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED;
    actor->destroy = PuppetDestroy;

    // Body only: the puppet neither deals nor takes damage yet (milestones 4 and 5). The draw code still registers
    // the sword and shield quads, so they are switched off too.
    p->cylinder.base.atFlags &= ~AT_ON;
    p->cylinder.base.acFlags &= ~AC_ON;
    p->shieldCylinder.base.atFlags &= ~AT_ON;
    p->shieldCylinder.base.acFlags &= ~AC_ON;
    p->shieldCylinder.base.ocFlags1 = OC1_NONE;
    for (ColliderQuad* quad : { &p->meleeWeaponQuads[0], &p->meleeWeaponQuads[1], &p->shieldQuad }) {
        quad->base.atFlags &= ~AT_ON;
        quad->base.acFlags &= ~AC_ON;
        quad->base.ocFlags1 = OC1_NONE;
    }

    // Name and color over the remote Link (removed automatically when the actor is destroyed).
    for (const PlayerInfo& info : GetSnapshot().players) {
        if (info.id == playerId) {
            NameTagOptions options = {};
            options.tag = kNameTagTag;
            options.yOffset = 0;
            options.textColor = { info.color.r, info.color.g, info.color.b, 255 };
            options.noZBuffer = 0;
            NameTag_RegisterForActorWithOptions(actor, info.name.c_str(), options);
        }
    }

    // The secondary init path skips this allocation; mask drawing binds it, as Kafei's does.
    p->maskObjectSegment = ZeldaArena_Malloc(0x3800);

    entry.actor = p;
    entry.form = s.form;
    entry.modelGroup = 0xFF;
    const uintptr_t ptr = reinterpret_cast<uintptr_t>(actor);
    sActorHooks[actor] = {
        playerId,
        GameInteractor::Instance->RegisterGameHookForPtr<GameInteractor::ShouldActorUpdate>(ptr, OnShouldUpdate),
        GameInteractor::Instance->RegisterGameHookForPtr<GameInteractor::ShouldActorDraw>(ptr, BeforeDraw),
        GameInteractor::Instance->RegisterGameHookForPtr<GameInteractor::OnActorDraw>(ptr, AfterDraw),
    };
    SPDLOG_INFO("[Coop] Puppet created for player {}: form {} at ({:.0f}, {:.0f}, {:.0f}) in scene {}", playerId,
                s.form, s.pos.x, s.pos.y, s.pos.z, play->sceneId);
    return true;
}

// Player_Init kills a secondary Link whose form object is not in the scene's object list. In the port every asset
// is loaded by name, so while a puppet spawns the five form objects are reported as present.
void OnObjectDependency(GIVanillaBehavior, bool* should, va_list originalArgs) {
    if (!sSpawning) {
        return;
    }
    va_list args;
    va_copy(args, originalArgs);
    const s16 objectId = static_cast<s16>(va_arg(args, s32));
    va_end(args);
    for (int form = 0; form < PLAYER_FORM_MAX; form++) {
        if (gPlayerFormObjectIds[form] == objectId) {
            *should = false;
        }
    }
}

bool CanSpawnNow() {
    // Never in the middle of a scene change or while the local Link is still being created.
    Player* local = GET_PLAYER(gPlayState);
    return local != nullptr && gPlayState->transitionMode == TRANS_MODE_OFF && local->actor.update != nullptr;
}

// --coop-test-puppet-log: one line per second with the local Link and every puppet, for probar.ps1.
void LogSamples() {
    static int64_t sLastSecond = 0;
    const int64_t second =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (!GetLaunchOptions().testPuppetLog || second == sLastSecond) {
        return;
    }
    // While a client switches into the session the old gamestate (e.g. the title demo) is still running.
    if (!IsHosting() && !IsClientPlayingInSession()) {
        return;
    }
    sLastSecond = second;
    Player* local = GET_PLAYER(gPlayState);
    const uint32_t selfId = IsHosting() ? kHostPlayerId : GetSnapshot().localId;
    SPDLOG_INFO("[Coop] Self sample: player {} form {} saveForm {} mask {} pos ({:.1f}, {:.1f}, {:.1f}) scene {}",
                selfId, local->transformation, gSaveContext.save.playerForm, local->currentMask,
                local->actor.world.pos.x, local->actor.world.pos.y, local->actor.world.pos.z, gPlayState->sceneId);
    for (const auto& [id, entry] : sPuppets) {
        if (entry.actor != nullptr) {
            const Vec3f& pos = entry.actor->actor.world.pos;
            SPDLOG_INFO("[Coop] Puppet sample: of player {} form {} mask {} pos ({:.1f}, {:.1f}, {:.1f}) scene {} "
                        "delay {}",
                        id, entry.actor->transformation, entry.actor->currentMask, pos.x, pos.y, pos.z,
                        gPlayState->sceneId, entry.sample.delayMs);
        }
    }
}

void OnPlayDestroy() {
    // The scene's actors are being deleted (our destroy runs for each puppet); start fresh in the next scene.
    sPuppets.clear();
}

void RegisterPuppetHooks() {
    REGISTER_VB_SHOULD(VB_ENABLE_OBJECT_DEPENDENCY, { OnObjectDependency(_, should, originalArgs); });
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayDestroy>(OnPlayDestroy);
}

} // namespace

void Frame() {
    // A puppet draw that was skipped after BeforeDraw would leave the swap in place; never let it reach the game.
    if (sDrawSwap.active) {
        SPDLOG_WARN("[Coop] Puppet draw swap was not restored; restoring");
        RestoreDrawSwap();
    }
    if (!SessionActive() || !InGameplay() || (!IsHosting() && !IsClientPlayingInSession())) {
        return;
    }
    std::vector<PlayerSync::Sample> samples = PlayerSync::SampleRemotes();
    std::unordered_set<uint32_t> seen;
    for (const PlayerSync::Sample& sample : samples) {
        const PlayerSync::State& s = sample.state;
        seen.insert(s.playerId);
        Entry& entry = sPuppets[s.playerId];
        const bool here =
            s.inGameplay && s.sceneId == gPlayState->sceneId && s.sceneLayer == (int8_t)gSaveContext.sceneLayer;
        if (!here) {
            Kill(s.playerId, entry, "player is in another scene");
            entry.haveSample = false;
            continue;
        }
        if (entry.actor != nullptr && entry.form != s.form) {
            SPDLOG_INFO("[Coop] Player {} changed form {} -> {}; recreating its puppet", s.playerId, entry.form,
                        s.form);
            Kill(s.playerId, entry, "form change");
        }
        if (entry.actor == nullptr && (!CanSpawnNow() || !Spawn(s.playerId, entry, s))) {
            continue;
        }
        entry.sample = sample;
        entry.haveSample = true;
    }
    for (auto& [id, entry] : sPuppets) {
        if (seen.count(id) == 0) {
            Kill(id, entry, "player left");
        }
    }
    LogSamples();
}

static RegisterShipInitFunc initFunc(RegisterPuppetHooks);

} // namespace Coop::Puppet
