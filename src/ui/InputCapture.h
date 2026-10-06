#pragma once

#include "runtime/CodeDetour.h"

#include <cstddef>

namespace UI::InputCapture
{
    inline constexpr std::size_t kTrampolineSize = CodeDetour::kCallDetourTrampolineSize;

    void Install();

    [[nodiscard]] float ForwardToImGui();
}
