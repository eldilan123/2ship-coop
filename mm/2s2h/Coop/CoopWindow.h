#ifndef COOP_WINDOW_H
#define COOP_WINDOW_H

#include <ship/window/gui/GuiWindow.h>
#include "CoopProtocol.h"

class CoopWindow : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

    void InitElement() override;
    void DrawElement() override;
    void UpdateElement() override{};

  private:
    char mAddress[128] = {};
    char mName[Coop::Protocol::kMaxNameLength + 1] = {};
};

#endif // COOP_WINDOW_H
