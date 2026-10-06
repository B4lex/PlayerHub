#pragma once

#include "ui/Window.h"

#include <memory>

namespace UI
{
    [[nodiscard]] std::unique_ptr<Window> CreateSettingsWindow();
}
