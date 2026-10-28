// Client: dying in the host's world (milestone 5, provisional until milestone 8). There is no game over for a client:
// the Link falls, and 5 s later it is back next to the host with 3 hearts and 3 s of invulnerability. Every cause of
// death ends in the same place of Player (health 0 -> func_80831F34), which either starts the game over or, with a
// fairy in a bottle, the fairy's revival. The bottle is shared (milestone 6): the revival waits for the host to take
// the fairy out of the shared bottle (500 ms at most); if the host says no (it was already used) or does not answer in
// time, the Link dies as without a fairy.
// The game over is stopped on the frame it starts, right after the Link's update and before GameOver_Update runs:
// nothing of it (music, letterbox, timers, saving) happens. Nothing is ever written to disk (CoopSaveGuard).
#include "CoopClientDeath.h"

#include <algorithm>
#include <chrono>
#include <spdlog/spdlog.h>
#include "Coop.h"
#include "CoopInventory.h"
#include "CoopSession.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "functions.h"
#include "variables.h"
#include "z64shrink_window.h"
#include "seqcmd.h"
// code_8019AF00.c: the fanfare Audio_PlayFanfare left pending (it starts a few frames later).
extern u8 sFanfareState;
}

namespace Coop::ClientDeath {

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr auto kDownTime = std::chrono::seconds(5);
constexpr auto kInvulnerableTime = std::chrono::seconds(3);
constexpr s16 kReviveHealth = 3 * 16; // Three hearts

bool sDown = false;
SteadyClock::time_point sDownSince;
bool sRespawning = false;
SteadyClock::time_point sInvulnerableUntil;
bool sInvulnerable = false;
bool sFairyLogged = false;
bool sCheckLogged = false; // The "1 s later" line of the current death
// The fairy's revival, held until the host takes the fairy out of the shared bottle.
enum class FairyHold { None, Waiting, Done };
FairyHold sFairy = FairyHold::None;
uint32_t sFairySeq = 0;
SteadyClock::time_point sFairySince;
constexpr auto kFairyMaxWait = std::chrono::milliseconds(500);

void StopGameOverFanfare() {
    sFanfareState = 0;
    if (AudioSeq_GetActiveSeqId(SEQ_PLAYER_FANFARE) == NA_BGM_GAME_OVER) {
        SEQCMD_STOP_SEQUENCE(SEQ_PLAYER_FANFARE, 0);
    }
}

bool ClientPlaying() {
    return !IsHosting() && IsClientPlayingInSession() && gPlayState != nullptr &&
           gSaveContext.gameMode == GAMEMODE_NORMAL;
}

void OnPlayerUpdate(Actor* actor) {
    if (!ClientPlaying() || actor != &GET_PLAYER(gPlayState)->actor) {
        return;
    }
    PlayState* play = gPlayState;
    Player* player = reinterpret_cast<Player*>(actor);
    if (play->gameOverCtx.state == GAMEOVER_INACTIVE && !(player->stateFlags1 & PLAYER_STATE1_DEAD)) {
        sFairyLogged = false;
        sFairy = FairyHold::None;
        return;
    }
    if (play->gameOverCtx.state == GAMEOVER_REVIVE_START && sFairy == FairyHold::None) {
        // The bottle just lost its fairy: that change goes to the host now, and the revival waits for its answer.
        sFairySeq = Inventory::SendNow();
        if (sFairySeq != 0) {
            sFairy = FairyHold::Waiting;
            sFairySince = SteadyClock::now();
            play->gameOverCtx.state = GAMEOVER_INACTIVE;
            SPDLOG_INFO("[Coop] client-death: a fairy from the shared bottle; waiting for the host (seq {})",
                        sFairySeq);
            return;
        }
        sFairy = FairyHold::Done;
    }
    if (play->gameOverCtx.state == GAMEOVER_REVIVE_START && !sFairyLogged) {
        sFairyLogged = true;
        SPDLOG_INFO("[Coop] client-death: the Link is revived by a fairy from its bottle (in memory)");
        return;
    }
    if (play->gameOverCtx.state != GAMEOVER_DEATH_START || sDown) {
        return;
    }
    // The game over starts this frame: stop it before GameOver_Update sees it.
    play->gameOverCtx.state = GAMEOVER_INACTIVE;
    // The death stopped every sequence (Audio_StopFanfare) and left the game over fanfare pending: cancel it and
    // play the scene's music again, as entering the scene does (seqId is "disabled" now, so it starts over).
    StopGameOverFanfare();
    Audio_SetBgmVolumeOn();
    gSaveContext.seqId = NA_BGM_DISABLED;
    gSaveContext.ambienceId = AMBIENCE_ID_DISABLED;
    Environment_PlaySceneSequence(play);
    ShrinkWindow_Letterbox_SetSizeTarget(0);
    // The Link stays down; its health comes back now so that any scene load from here on starts it alive.
    gSaveContext.save.saveInfo.playerData.health =
        std::min<s16>(kReviveHealth, gSaveContext.save.saveInfo.playerData.healthCapacity);
    sDown = true;
    sCheckLogged = false;
    sDownSince = SteadyClock::now();
    SPDLOG_INFO("[Coop] client-death: the Link died in scene {}; game over cut, back next to the host in 5 s",
                play->sceneId);
}

void OnSceneInit(s8, s8) {
    if (sRespawning && ClientPlaying()) {
        sRespawning = false;
        sInvulnerable = true;
        sInvulnerableUntil = SteadyClock::now() + kInvulnerableTime;
        SPDLOG_INFO("[Coop] client-death: back next to the host with {} health, invulnerable for 3 s",
                    gSaveContext.save.saveInfo.playerData.health);
    }
}

void RegisterClientDeathHooks() {
    auto* gi = GameInteractor::Instance;
    gi->RegisterGameHookForID<GameInteractor::OnActorUpdate>(ACTOR_PLAYER, OnPlayerUpdate);
    gi->RegisterGameHook<GameInteractor::OnSceneInit>(OnSceneInit);
}

} // namespace

void Frame() {
    if (IsHosting() || !IsClientInSession()) {
        sDown = sRespawning = sInvulnerable = false;
        sFairy = FairyHold::None;
        return;
    }
    if (!ClientPlaying()) {
        return;
    }
    const auto now = SteadyClock::now();
    if (sFairy == FairyHold::Waiting) {
        const Inventory::Answer answer = Inventory::AnswerFor(sFairySeq);
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(now - sFairySince).count();
        if (answer == Inventory::Answer::Applied) {
            sFairy = FairyHold::Done;
            gPlayState->gameOverCtx.state = GAMEOVER_REVIVE_START;
            SPDLOG_INFO("[Coop] client-death: the host took the fairy out of the shared bottle after {} ms; revived",
                        waited);
        } else if (answer == Inventory::Answer::Rejected || now - sFairySince >= kFairyMaxWait) {
            if (answer != Inventory::Answer::Rejected) {
                Inventory::Cancel(sFairySeq);
            }
            sFairy = FairyHold::Done;
            gPlayState->gameOverCtx.state = GAMEOVER_DEATH_START;
            SPDLOG_INFO("[Coop] client-death: no fairy after all ({} after {} ms); the Link dies",
                        answer == Inventory::Answer::Rejected ? "the host refused it, already used" : "no answer",
                        waited);
        }
    }
    if (sDown) {
        StopGameOverFanfare(); // In case it got through anyway
    }
    // What is left of the game over a second later: nothing (its music is NA_BGM_GAME_OVER 0x20 on the fanfare player).
    if (sDown && !sCheckLogged && now - sDownSince >= std::chrono::seconds(1)) {
        sCheckLogged = true;
        SPDLOG_INFO("[Coop] client-death: 1 s later: game over state {}, fanfare seq 0x{:04X}, main seq 0x{:04X}, "
                    "letterbox {}",
                    static_cast<int>(gPlayState->gameOverCtx.state), AudioSeq_GetActiveSeqId(SEQ_PLAYER_FANFARE),
                    AudioSeq_GetActiveSeqId(SEQ_PLAYER_BGM_MAIN), ShrinkWindow_Letterbox_GetSizeTarget());
    }
    if (sDown && now - sDownSince >= kDownTime && gPlayState->transitionTrigger == TRANS_TRIGGER_OFF &&
        gPlayState->transitionMode == TRANS_MODE_OFF) {
        sDown = false;
        sRespawning = true;
        ClientReturnToHost("the Link died");
    }
    if (sInvulnerable) {
        Player* player = GET_PLAYER(gPlayState);
        if (now >= sInvulnerableUntil || player == nullptr) {
            sInvulnerable = false;
        } else if (player->invincibilityTimer < 2) {
            player->invincibilityTimer = 2; // Counts down by one per frame: kept above 0 for 3 s
        }
    }
}

bool IsDown() {
    return sDown;
}

static RegisterShipInitFunc initFunc(RegisterClientDeathHooks);

} // namespace Coop::ClientDeath
