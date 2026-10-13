#include "ui/hud/GameView.h"

#include <RE/Skyrim.h>

namespace UI
{
    bool IsGameViewShown()
    {
        auto* ui = RE::UI::GetSingleton();
        return ui && ui->IsShowingMenus() && !ui->GameIsPaused() && !ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME)
            && !ui->IsMenuOpen(RE::RaceSexMenu::MENU_NAME);
    }
}
