#pragma once

#include "ui/Window.h"

#include <memory>

namespace UI
{
    // Edits the plugin configuration in the middle of the screen, one tab per
    // file section. [UI] SettingsKey opens it during gameplay. An edit changes
    // the live settings and saves skyrim-mp.ini at once, except in the Default
    // Outfit tab, where Apply does and closing discards what was not applied.
    // Uses the mouse.
    [[nodiscard]] std::unique_ptr<Window> CreateSettingsWindow();
}
