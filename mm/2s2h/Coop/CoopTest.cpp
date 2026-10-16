// Test-only actions driven by --coop-test-* / --coop-autoload-slot, so the coop flows can be exercised without a
// keyboard: probar.ps1 starts the games with these options and checks the "[Coop]" log lines.
#include "CoopTest.h"

#include <chrono>
#include <spdlog/spdlog.h>
#include "Coop.h"
#include "CoopArgs.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "overlays/gamestates/ovl_select/z_select.h"
}

namespace Coop::Test {

namespace {

bool sBootHandled = false;

// --coop-test-debug-save: like WarpPoint's boot-to-point, but on the game's debug save (fileNum 0xFF), which lives
// only in memory and is never written.
void BootIntoDebugSave(uint16_t entrance) {
    SPDLOG_INFO("[Coop] TEST: booting into gameplay on the debug save at entrance 0x{:04X}", entrance);
    gSaveContext.gameMode = GAMEMODE_NORMAL;
    gSaveContext.fileNum = 0xFF;
    MapSelect_LoadGame((MapSelectState*)gGameState, entrance, 0);
}

void OnConsoleLogoUpdate() {
    if (sBootHandled) {
        return;
    }
    const LaunchOptions& options = GetLaunchOptions();
    if (options.testDebugSaveEntrance.has_value()) {
        sBootHandled = true;
        BootIntoDebugSave(*options.testDebugSaveEntrance);
    }
}

void RegisterTestHooks() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnConsoleLogoUpdate>(OnConsoleLogoUpdate);
}

} // namespace

void Frame() {
}

static RegisterShipInitFunc initFunc(RegisterTestHooks);

} // namespace Coop::Test
