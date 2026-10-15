#pragma once

#include <imgui.h>

#include <optional>

namespace UI::Backdrop
{
    // The game's finished frame, blurred, stretched over the display.
    struct Blur
    {
        ImTextureRef texture;
        ImU32 tint{ IM_COL32_WHITE };
        ImVec2 uvOrigin;
        ImVec2 uvPerPixel;

        [[nodiscard]] ImVec2 ToUv(const ImVec2& a_position) const
        {
            return ImVec2{ uvOrigin.x + a_position.x * uvPerPixel.x, uvOrigin.y + a_position.y * uvPerPixel.y };
        }
    };

    // Covers the display behind every overlay window with the game's finished frame, blurred and
    // darkened, or only darkened while the blur is unavailable. Once per frame at most.
    void Draw();

    // Blurs the frame at most once per frame; empty while the blur is unavailable.
    [[nodiscard]] std::optional<Blur> GetBlur();

    void DrawBlur(ImDrawList& a_list, const ImVec2& a_min, const ImVec2& a_max, float a_rounding, float a_alpha);

    // After ImGui::Render: puts the blurred frame under each translucent popup and tooltip.
    void BlurPopups(ImDrawData& a_drawData);

    // Before ImGui::NewFrame: frees the textures when nothing used the blur last frame.
    void Update();
}
