#pragma once

#include "runtime/CodeDetour.h"

#include <cstddef>

namespace UI::InputCapture
{
    inline constexpr std::size_t kTrampolineSize = CodeDetour::kCallDetourTrampolineSize;

    void Install();

    [[nodiscard]] float ForwardToImGui();

    // Whether the focused window takes the mouse this frame, as ForwardToImGui last decided.
    [[nodiscard]] bool ShowsCursor();
}
