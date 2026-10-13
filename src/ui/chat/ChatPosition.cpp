#include "ui/chat/ChatPosition.h"
#include "config/PluginConfig.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace UI
{
    namespace
    {
        using Config::UISettings;

        ImVec2 ToCorner(std::uint32_t a_horizontal, std::uint32_t a_vertical)
        {
            const auto& display = ImGui::GetIO().DisplaySize;
            return ImVec2{ std::floor(static_cast<float>(a_horizontal) * 0.01F * display.x),
                std::floor(display.y - static_cast<float>(a_vertical) * 0.01F * display.y) };
        }

        std::uint32_t ToPercent(float a_percent, bool a_nearest)
        {
            return static_cast<std::uint32_t>(std::clamp(a_nearest ? std::round(a_percent) : std::floor(a_percent),
                0.0F, static_cast<float>(UISettings::kMaximumChatPosition)));
        }
    }

    ImVec2 ChatPosition::Get() const
    {
        if (moved)
        {
            return corner;
        }
        const auto config = Config::Get();
        return ToCorner(config->ui.chatPositionHorizontal, config->ui.chatPositionVertical);
    }

    void ChatPosition::Follow(const ImVec2& a_frameSize)
    {
        if (!held)
        {
            return;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            Release();
            return;
        }
        // A click without a drag moves nothing.
        if (!moved && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            return;
        }
        moved = true;
        frameSize = a_frameSize;
        const auto& display = ImGui::GetIO().DisplaySize;
        const auto delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0F);
        // Whole pixels within whole-pixel bounds, so the room from the corner
        // holds the frame.
        corner = ImVec2{ std::clamp(std::round(origin.x + delta.x), 0.0F,
                             std::floor((std::max)(display.x - frameSize.x, 0.0F))),
            std::clamp(std::round(origin.y + delta.y), (std::min)(std::ceil(frameSize.y), display.y), display.y) };
    }

    void ChatPosition::Grab(const ImVec2& a_corner)
    {
        if (held)
        {
            if (moved)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            }
            return;
        }
        // The field is active while the player types, which would otherwise
        // keep the window from being hovered.
        constexpr auto kHover = ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_NoPopupHierarchy
            | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem;
        if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsAnyItemHovered()
            || !ImGui::IsWindowHovered(kHover))
        {
            return;
        }
        held = true;
        moved = false;
        origin = a_corner;
        corner = a_corner;
    }

    void ChatPosition::Release()
    {
        if (!held)
        {
            return;
        }
        held = false;
        if (!std::exchange(moved, false))
        {
            return;
        }

        const auto& display = ImGui::GetIO().DisplaySize;
        if (display.x <= 0.0F || display.y <= 0.0F)
        {
            return;
        }
        // The nearest percentage, unless its corner leaves the frame less room
        // than the drag did; the one toward the lower left corner never does.
        const ImVec2 exact{ corner.x / display.x * 100.0F, (display.y - corner.y) / display.y * 100.0F };
        const auto nearest = ToCorner(ToPercent(exact.x, true), ToPercent(exact.y, true));
        const auto horizontal = ToPercent(exact.x, nearest.x + frameSize.x <= display.x);
        const auto vertical = ToPercent(exact.y, nearest.y >= frameSize.y);

        const auto live = Config::Get();
        if (horizontal == live->ui.chatPositionHorizontal && vertical == live->ui.chatPositionVertical)
        {
            return;
        }
        auto config = *live;
        config.ui.chatPositionHorizontal = horizontal;
        config.ui.chatPositionVertical = vertical;
        const bool saved = Config::Apply(std::move(config));
        logs::debug("Chat moved to {}% across and {}% up{}", horizontal, vertical, saved ? "" : "; not saved");
    }
}
