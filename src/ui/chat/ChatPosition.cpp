#include "ui/chat/ChatPosition.h"
#include "config/PluginConfig.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace UI
{
    namespace
    {
        using Config::UISettings;

        // Rounded, so a corner stored from whole pixels comes back as the same pixels.
        ImVec2 ToCorner(float a_horizontal, float a_vertical)
        {
            const auto& display = ImGui::GetIO().DisplaySize;
            return ImVec2{ std::round(a_horizontal * 0.01F * display.x),
                std::round(display.y - a_vertical * 0.01F * display.y) };
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
        const auto& display = ImGui::GetIO().DisplaySize;
        const auto delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0F);
        // Whole pixels within whole-pixel bounds, so the room from the corner
        // holds the frame.
        corner = ImVec2{ std::clamp(std::round(origin.x + delta.x), 0.0F,
                             std::floor((std::max)(display.x - a_frameSize.x, 0.0F))),
            std::clamp(std::round(origin.y + delta.y), (std::min)(std::ceil(a_frameSize.y), display.y), display.y) };
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

        const auto live = Config::Get();
        auto config = *live;
        Store(config.ui, corner);
        if (config.ui == live->ui)
        {
            return;
        }
        const auto horizontal = config.ui.chatPositionHorizontal;
        const auto vertical = config.ui.chatPositionVertical;
        const bool saved = Config::Apply(std::move(config));
        logs::debug("Chat moved to {:.2f}% across and {:.2f}% up{}", horizontal, vertical, saved ? "" : "; not saved");
    }

    void ChatPosition::Store(Config::UISettings& a_ui, const ImVec2& a_corner)
    {
        const auto& display = ImGui::GetIO().DisplaySize;
        if (display.x <= 0.0F || display.y <= 0.0F)
        {
            return;
        }
        // A value that already puts the corner there stays, such as a slider's whole percent.
        const auto current = ToCorner(a_ui.chatPositionHorizontal, a_ui.chatPositionVertical);
        if (current.x != a_corner.x)
        {
            a_ui.chatPositionHorizontal
                = std::clamp(a_corner.x / display.x * 100.0F, 0.0F, UISettings::kMaximumChatPosition);
        }
        if (current.y != a_corner.y)
        {
            a_ui.chatPositionVertical
                = std::clamp((display.y - a_corner.y) / display.y * 100.0F, 0.0F, UISettings::kMaximumChatPosition);
        }
    }
}
