#pragma once

#include "ui/Window.h"

#include <memory>

namespace UI
{
    [[nodiscard]] std::unique_ptr<Window> CreateVoiceIndicator();

    // Positions are [Voice] IndicatorPositionHorizontal and IndicatorPositionVertical values.
    void PreviewVoiceIndicator(float a_horizontal, float a_vertical);
}
