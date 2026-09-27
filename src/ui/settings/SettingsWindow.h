#pragma once

#include "ui/Window.h"

#include <memory>

namespace UI
{
    // Edits the plugin configuration in the middle of the screen, one tab per
    // file section. [UI] SettingsKey opens it during gameplay. Apply changes the
    // live settings and saves skyrim-mp.ini; closing discards what was not
    // applied. Uses the mouse.
    [[nodiscard]] std::unique_ptr<Window> CreateSettingsWindow();
}
