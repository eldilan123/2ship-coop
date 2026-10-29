#include "CoopStory.h"

#include <chrono>
#include <string>
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include "Coop.h"
#include "CoopRooms.h"
#include "CoopSession.h"
#include "2s2h/BenGui/Notification.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
}

namespace Coop::Story {

namespace {

using SteadyClock = std::chrono::steady_clock;

// Audit (CoopStory.h): weekEventReg bits the game clears again that the interface, the Link or the message system
// read, or that depend on where each player stands.
struct LocalBit {
    u16 flag; // PACK_WEEKEVENTREG_FLAG
    const char* name;
    const char* reason;
};
constexpr LocalBit kLocalBits[] = {
    { WEEKEVENTREG_08_01, "08_01",
      "in a minigame (Honey & Darling, Romani's aliens): the interface limits the buttons" },
    { WEEKEVENTREG_31_80, "31_80", "riding with Cremia (Obj_Um): the interface limits the buttons" },
    { WEEKEVENTREG_KICKOUT_WAIT, "KICKOUT_WAIT", "being kicked out of a place at closing time: where each one stands" },
    { WEEKEVENTREG_KICKOUT_TIME_PASSED, "KICKOUT_TIME_PASSED", "the same kickout, its timer" },
    { WEEKEVENTREG_PLAYING_SWORDSMAN_MINIGAME, "PLAYING_SWORDSMAN_MINIGAME",
      "in the swordsman's minigame: the interface limits the buttons" },
    { WEEKEVENTREG_84_20, "84_20", "set by the Moon's children (En_Js), read by the interface" },
    { WEEKEVENTREG_90_20, "90_20", "in the jumping game (En_Jgame_Tsn): the interface limits the buttons" },
};

// The result of the alien defense: decided by the host's invasion only (the client's is not run).
constexpr u16 kHostOnlyBits[] = { WEEKEVENTREG_DEFENDED_AGAINST_ALIENS, WEEKEVENTREG_FAILED_TO_DEFEND_AGAINST_ALIENS };

// En_Invadepoh types that make up the invasion (handler, aliens, UFO, abductor): not run on the client.
bool IsInvasionPart(const Actor* a) {
    if (a->id != ACTOR_EN_INVADEPOH) {
        return false;
    }
    const int type = (a->params >> 4) & 0xF; // EN_INVADEPOH_GET_TYPE
    return type == 0 || type == 1 || type == 6 || type == 13;
}
// NPCs whose state only one game can run: the client cannot talk to them yet (priority of milestone 7).
struct Blocked {
    s16 id;
    const char* reason;
};
constexpr Blocked kBlocked[] = {
    { ACTOR_EN_SYATEKI_MAN, "shooting gallery: score, timer and targets live in one game" },
    { ACTOR_EN_TAKARAYA, "treasure chest shop: its maze and timer live in one game" },
    { ACTOR_EN_KENDO_JS, "swordsman's school: the minigame lives in one game" },
    { ACTOR_EN_FU, "Honey & Darling: the minigame lives in one game" },
    { ACTOR_EN_AOB_01, "dog race: the race lives in one game" },
    { ACTOR_EN_HS, "Grog's chicks: the chicks follow one Link" },
    { ACTOR_EN_DNO, "Deku Butler's race: the race reads the butler's position" },
    { ACTOR_EN_MA4, "Romani's alien defense: the minigame lives in one game" },
    { ACTOR_EN_AN, "Anju: her schedule and events (with Kafei) live in one game" },
    { ACTOR_EN_TEST3, "Kafei: his schedule and events live in one game" },
    { ACTOR_EN_BABA, "the bomb shop lady and the night robbery: one game's event" },
    { ACTOR_EN_SUTTARI, "Sakon and the night robbery: one game's event" },
};

int sCutscenesFromClient = 0;
int sStuck = 0;
SteadyClock::time_point sFrozenSince = {};
bool sFrozen = false;
int sBlocked = 0;
int sHostOnly = 0;
SteadyClock::time_point sBusyUntil = {};
const Actor* sBlockedTalk = nullptr;

bool ClientPlaying() {
    return !IsHosting() && IsClientPlayingInSession() && gPlayState != nullptr &&
           gSaveContext.gameMode == GAMEMODE_NORMAL;
}

void Notify(const std::string& message) {
    Notification::Emit({ .message = message });
}

const Blocked* FindBlocked(s16 id) {
    for (const Blocked& b : kBlocked) {
        if (b.id == id) {
            return &b;
        }
    }
    return nullptr;
}

void HostOnly(const char* what) {
    sHostOnly++;
    SPDLOG_INFO("[Coop] story-host-only: {} stopped on the client (only the host can)", what);
    Notify(std::string("Coop: solo el host puede ") + what);
}

void ClientFrame() {
    PlayState* play = gPlayState;
    Player* link = GET_PLAYER(play);
    if (link == nullptr) {
        return;
    }
    MessageContext& msg = play->msgCtx;
    const auto now = SteadyClock::now();

    // Talking, in a cutscene or in the pause menu: no hits from the host's enemies (they do not freeze for this game),
    // and 500 ms after; none build up for when the menu closes.
    const bool busy = msg.msgMode != MSGMODE_NONE || (link->stateFlags1 & PLAYER_STATE1_TALKING) ||
                      link->csAction != PLAYER_CSACTION_NONE || CutsceneManager_GetCurrentCsId() != CS_ID_NONE ||
                      IS_PAUSED(&play->pauseCtx);
    if (busy) {
        sBusyUntil = now + std::chrono::milliseconds(500);
    }
    if (now < sBusyUntil && link->invincibilityTimer < 2) {
        link->invincibilityTimer = 2; // Counts down by one per frame
    }

    // An NPC only one game can run: its dialogue closes.
    const Actor* talk = link->talkActor;
    if (msg.msgMode != MSGMODE_NONE && talk != nullptr) {
        if (const Blocked* b = FindBlocked(talk->id); b != nullptr && sBlockedTalk != talk) {
            sBlockedTalk = talk;
            sBlocked++;
            Message_CloseTextbox(play);
            SPDLOG_INFO("[Coop] story-blocked: actor 0x{:03X} ({}); its dialogue closed on the client", talk->id,
                        b->reason);
            Notify("Coop: esto todavia lo hace solo el host (hito 7)");
        }
    } else if (msg.msgMode == MSGMODE_NONE) {
        sBlockedTalk = nullptr;
    }

    // The host's: the Song of Time (and its inverted and double forms), the Song of Soaring's flight, the owl's save.
    const u16 mode = msg.ocarinaMode;
    if (mode == OCARINA_MODE_PROCESS_SOT || mode == OCARINA_MODE_PROCESS_INVERTED_TIME ||
        mode == OCARINA_MODE_PROCESS_DOUBLE_TIME) {
        msg.ocarinaMode = OCARINA_MODE_END;
        Message_CloseTextbox(play);
        HostOnly("cambiar el tiempo con la ocarina");
    } else if (mode == OCARINA_MODE_WARP ||
               (mode >= OCARINA_MODE_WARP_TO_GREAT_BAY_COAST && mode <= OCARINA_MODE_WARP_TO_ENTRANCE)) {
        msg.ocarinaMode = OCARINA_MODE_END;
        Message_CloseTextbox(play);
        HostOnly("volar con la Cancion del Vuelo");
    }
    if (msg.msgMode == MSGMODE_OWL_SAVE_0 || msg.msgMode == MSGMODE_NEW_CYCLE_0) {
        Message_CloseTextbox(play);
        HostOnly(msg.msgMode == MSGMODE_OWL_SAVE_0 ? "guardar con el buho" : "volver al primer dia");
    }
}

// Stuck detector (milestone 7): the Link frozen (cutscene wait, PLAYER_STATE1_20000000 / 200 or a csAction) for 15 s
// while nothing that freezes it is going on: no cutscene, text, pause, transition, room move, held exit or death. The
// client then goes back next to the host (gCoop.StuckRecover); the host only logs it.
void CheckStuck() {
    PlayState* play = gPlayState;
    Player* link = GET_PLAYER(play);
    if (link == nullptr) {
        return;
    }
    const bool frozen = (link->stateFlags1 & (PLAYER_STATE1_20000000 | PLAYER_STATE1_200)) != 0 ||
                        link->csAction != PLAYER_CSACTION_NONE;
    const bool explained = play->msgCtx.msgMode != MSGMODE_NONE || CutsceneManager_GetCurrentCsId() != CS_ID_NONE ||
                           play->csCtx.state != CS_STATE_IDLE || IS_PAUSED(&play->pauseCtx) ||
                           play->transitionTrigger != TRANS_TRIGGER_OFF || play->transitionMode != TRANS_MODE_OFF ||
                           (link->stateFlags1 & PLAYER_STATE1_DEAD) || Rooms::MoveInProgress() ||
                           (!IsHosting() && HoldingExit());
    const auto now = SteadyClock::now();
    if (!frozen || explained) {
        sFrozen = false;
        return;
    }
    if (!sFrozen) {
        sFrozen = true;
        sFrozenSince = now;
        return;
    }
    if (now - sFrozenSince < std::chrono::seconds(15)) {
        return;
    }
    sFrozen = false;
    sStuck++;
    SPDLOG_WARN("[Coop] stuck-recover: the Link is frozen with nothing going on (state1 0x{:08X}, csAction {}){}",
                link->stateFlags1, link->csAction,
                IsHosting() ? "; the host does nothing about it" : "; going back next to the host");
    if (!IsHosting() && CVarGetInteger(CVAR_COOP_STUCK_RECOVER, 1) != 0) {
        Notify("Coop: tu Link quedo trabado; volviendo junto al host");
        ClientReturnToHost("the Link was stuck");
    }
}

void LogStats() {
    static SteadyClock::time_point sNext = {};
    const auto now = SteadyClock::now();
    if (now < sNext) {
        return;
    }
    sNext = now + std::chrono::seconds(10);
    SPDLOG_INFO("[Coop] story-stats: cutscene-from-b {}, story-blocked {}, story-host-only {}, stuck-recover {}",
                sCutscenesFromClient, sBlocked, sHostOnly, sStuck);
}

void OnFrame() {
    if (ClientPlaying()) {
        ClientFrame();
        CheckStuck();
        LogStats();
    } else if (IsHosting() && gPlayState != nullptr && gSaveContext.gameMode == GAMEMODE_NORMAL) {
        CheckStuck();
    }
}

void OnActorInit(Actor* a) {
    if (ClientPlaying() && IsInvasionPart(a)) {
        SPDLOG_INFO(
            "[Coop] aliens: En_Invadepoh params 0x{:04X} removed on the client; the host runs the alien defense",
            static_cast<u16>(a->params));
        Actor_Kill(a);
    }
}

void RegisterStoryHooks() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameStateMainStart>(OnFrame);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnActorInit>(OnActorInit);
    // A cutscene the client starts runs on the client; the flags and items it writes reach the host.
    REGISTER_VB_SHOULD(VB_START_CUTSCENE, {
        if (!ClientPlaying() || !*should) {
            return;
        }
        const s16 csId = *va_arg(args, s16*);
        Actor* actor = va_arg(args, Actor*);
        if (csId > CS_ID_NONE && CutsceneManager_GetCurrentCsId() == CS_ID_NONE) {
            sCutscenesFromClient++;
            SPDLOG_INFO("[Coop] cutscene-from-b: cutscene {} ({}) runs on the client; what it writes goes to the host",
                        csId, actor != nullptr ? fmt::format("actor 0x{:03X}", actor->id) : std::string("no actor"));
        }
    });
    // NPCs that skip time (Pierre's dance, Anju's grandmother's story): the clock is the host's.
    REGISTER_VB_SHOULD(VB_SCARECROW_DANCE_SET_TIME, {
        if (ClientPlaying() && *should) {
            *should = false;
            HostOnly("adelantar el tiempo con el espantapajaros");
        }
    });
    REGISTER_VB_SHOULD(VB_GRANNY_STORY_INCREMENT_DAY, {
        if (ClientPlaying() && *should) {
            *should = false;
            HostOnly("pasar al dia siguiente con la abuela");
        }
    });
}

} // namespace

uint8_t LocalWeekEventBits(int index) {
    uint8_t mask = 0;
    for (const LocalBit& b : kLocalBits) {
        if ((b.flag >> 8) == index) {
            mask |= static_cast<uint8_t>(b.flag & 0xFF);
        }
    }
    return mask;
}

uint8_t HostOnlyWeekEventBits(int index) {
    uint8_t mask = 0;
    for (u16 flag : kHostOnlyBits) {
        if ((flag >> 8) == index) {
            mask |= static_cast<uint8_t>(flag & 0xFF);
        }
    }
    return mask;
}

bool FromClient() {
    return CVarGetInteger(CVAR_COOP_STORY_FROM_CLIENT, 1) != 0;
}

bool ClientBusy() {
    return SteadyClock::now() < sBusyUntil;
}

static RegisterShipInitFunc initFunc(RegisterStoryHooks);

} // namespace Coop::Story
