#pragma once

#include <imgui.h>

#include <cstdint>

namespace UI::ItemPreview
{
    void Draw(std::uint32_t a_formID, ImVec2 a_size);

    void Update();
}
