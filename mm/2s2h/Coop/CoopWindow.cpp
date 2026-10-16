#include "CoopWindow.h"

#include <algorithm>
#include <cstring>
#include <imgui.h>
#include <fmt/format.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>
#include "Coop.h"
#include "2s2h/BenGui/BenMenu.h"
#include "2s2h/ShipInit.hpp"

#define CVAR_COOP_WINDOW "gWindows.Coop"
#define COOP_WINDOW_NAME "Coop"

namespace BenGui {
extern std::shared_ptr<BenMenu> mBenMenu;
}

namespace {

void SaveCVars() {
    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

ImVec4 StateColor(Coop::SessionState state) {
    switch (state) {
        case Coop::SessionState::Hosting:
        case Coop::SessionState::Connected:
            return ImVec4(0.4f, 0.9f, 0.4f, 1.0f);
        case Coop::SessionState::Connecting:
        case Coop::SessionState::Handshaking:
            return ImVec4(0.95f, 0.85f, 0.3f, 1.0f);
        case Coop::SessionState::Rejected:
        case Coop::SessionState::Disconnected:
        case Coop::SessionState::Failed:
            return ImVec4(0.95f, 0.4f, 0.4f, 1.0f);
        default:
            return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
    }
}

Coop::Protocol::Color ConfiguredColor() {
    const Color_RGB8 color = CVarGetColor24(CVAR_COOP_COLOR, { 255, 255, 255 });
    return { color.r, color.g, color.b };
}

} // namespace

void CoopWindow::InitElement() {
    strncpy(mAddress, CVarGetString(CVAR_COOP_ADDRESS, "127.0.0.1"), sizeof(mAddress) - 1);
    strncpy(mName, CVarGetString(CVAR_COOP_NAME, "Player"), sizeof(mName) - 1);
}

void CoopWindow::DrawElement() {
    const Coop::SessionSnapshot snapshot = Coop::GetSnapshot();
    const bool running = Coop::IsRunning();
    const int mode = CVarGetInteger(CVAR_COOP_MODE, 0);
    int port = CVarGetInteger(CVAR_COOP_PORT, Coop::kDefaultPort);

    ImGui::BeginDisabled(running);
    if (ImGui::RadioButton("Host", mode == 0)) {
        CVarSetInteger(CVAR_COOP_MODE, 0);
        SaveCVars();
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Join", mode == 1)) {
        CVarSetInteger(CVAR_COOP_MODE, 1);
        SaveCVars();
    }
    if (mode == 1 && ImGui::InputText("Address", mAddress, sizeof(mAddress))) {
        CVarSetString(CVAR_COOP_ADDRESS, mAddress);
        SaveCVars();
    }
    if (ImGui::InputInt("Port", &port)) {
        CVarSetInteger(CVAR_COOP_PORT, std::clamp(port, 1, 65535));
        SaveCVars();
    }
    if (ImGui::InputText("Name", mName, sizeof(mName))) {
        CVarSetString(CVAR_COOP_NAME, mName);
        SaveCVars();
    }
    const Coop::Protocol::Color color = ConfiguredColor();
    float colorEdit[3] = { color.r / 255.0f, color.g / 255.0f, color.b / 255.0f };
    if (ImGui::ColorEdit3("Color", colorEdit)) {
        CVarSetColor24(CVAR_COOP_COLOR, { static_cast<uint8_t>(colorEdit[0] * 255.0f + 0.5f),
                                          static_cast<uint8_t>(colorEdit[1] * 255.0f + 0.5f),
                                          static_cast<uint8_t>(colorEdit[2] * 255.0f + 0.5f) });
        SaveCVars();
    }
    ImGui::EndDisabled();

    if (!running) {
        if (ImGui::Button(mode == 0 ? "Start hosting" : "Join session")) {
            const uint16_t sessionPort = static_cast<uint16_t>(CVarGetInteger(CVAR_COOP_PORT, Coop::kDefaultPort));
            if (mode == 0) {
                Coop::StartHost(sessionPort, mName, ConfiguredColor());
            } else {
                Coop::StartJoin(mAddress, sessionPort, mName, ConfiguredColor());
            }
        }
    } else if (ImGui::Button(snapshot.role == Coop::Role::Host ? "Stop hosting" : "Leave session")) {
        Coop::Stop();
    }

    ImGui::Separator();
    ImGui::Text("Build: %s", Coop::BuildVersion().c_str());
    ImGui::TextColored(StateColor(snapshot.state), "Status: %s", Coop::SessionStateName(snapshot.state));
    if (!snapshot.message.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(snapshot.message.c_str());
        ImGui::PopTextWrapPos();
    }

    if (snapshot.players.empty()) {
        return;
    }
    ImGui::Separator();
    ImGui::Text("Players (%d/%d)", static_cast<int>(snapshot.players.size()), static_cast<int>(Coop::kMaxPlayers));
    if (ImGui::BeginTable("CoopPlayers", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("##color");
        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Ping");
        for (const auto& player : snapshot.players) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const ImVec4 swatch(player.color.r / 255.0f, player.color.g / 255.0f, player.color.b / 255.0f, 1.0f);
            ImGui::ColorButton(fmt::format("##coopColor{}", player.id).c_str(), swatch,
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(14, 14));
            ImGui::TableNextColumn();
            const std::string label = fmt::format("{}{}{}", player.name, player.isHost ? " (host)" : "",
                                                  player.id == snapshot.localId ? " (you)" : "");
            ImGui::TextUnformatted(label.c_str());
            ImGui::TableNextColumn();
            if (player.rttMs >= 0) {
                ImGui::Text("%d ms", player.rttMs);
            } else {
                ImGui::TextDisabled("-");
            }
        }
        ImGui::EndTable();
    }
}

static void RegisterCoopWindow() {
    auto gui = Ship::Context::GetRawInstance()->GetWindow()->GetGui();
    gui->AddGuiWindow(std::make_shared<CoopWindow>(CVAR_COOP_WINDOW, COOP_WINDOW_NAME, ImVec2(380, 360)));
}

static void RegisterCoopMenu() {
    auto& menu = BenGui::mBenMenu;
    menu->AddMenuEntry("Coop", "gSettings.Menu.CoopSidebarSection");
    menu->AddSidebarEntry("Coop", "Session", 1);
    WidgetPath path = { "Coop", "Session", SECTION_COLUMN_1 };
    menu->AddWidget(path, "Popout Coop Window", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_COOP_WINDOW)
        .WindowName(COOP_WINDOW_NAME);
}

static RegisterShipInitFunc initWindow(RegisterCoopWindow);
static RegisterMenuInitFunc initMenu(RegisterCoopMenu);
