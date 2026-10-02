#include "ui/chat/ChatSize.h"
#include "config/PluginConfig.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace UI
{
    namespace
    {
        using Config::UISettings;

        std::uint32_t Limit(std::uint32_t a_value, std::uint32_t a_minimum, std::uint32_t a_maximum)
        {
            return std::clamp(a_value, a_minimum, (std::max)(a_minimum, a_maximum));
        }

        ChatSize::Extent Limit(const ChatSize::Extent& a_size, const ChatSize::Extent& a_maximum)
        {
            return ChatSize::Extent{
                .width = Limit(a_size.width, UISettings::kMinimumChatWidth,
                    (std::min)(a_maximum.width, UISettings::kMaximumChatWidth)),
                .lines = Limit(a_size.lines, UISettings::kMinimumChatLines,
                    (std::min)(a_maximum.lines, UISettings::kMaximumChatLines)),
            };
        }

        // a_origin moved by a_steps, to the nearest whole one.
        std::uint32_t Move(std::uint32_t a_origin, float a_steps)
        {
            return static_cast<std::uint32_t>((std::max)(static_cast<float>(a_origin) + std::round(a_steps), 0.0F));
        }
    }

    void ChatSize::ShowResizeCursor(std::uint8_t a_edges)
    {
        ImGui::SetMouseCursor(a_edges == kTop ? ImGuiMouseCursor_ResizeNS
                : a_edges == kRight           ? ImGuiMouseCursor_ResizeEW
                                              : ImGuiMouseCursor_ResizeNESW);
    }

    ChatSize::Extent ChatSize::Get(const Extent& a_maximum) const
    {
        if (edges != kNone)
        {
            return Limit(size, a_maximum);
        }
        const auto config = Config::Get();
        return Limit(Extent{ .width = config->ui.chatWidth, .lines = config->ui.chatLines }, a_maximum);
    }

    std::uint8_t ChatSize::FindEdges(const Grip& a_grip)
    {
        // The field is active while the player types, which would otherwise
        // keep the window from being hovered.
        if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
        {
            return kNone;
        }
        const auto mouse = ImGui::GetMousePos();
        const auto top = ImGui::GetWindowPos().y;
        const auto right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth();
        const bool onTop = mouse.y < top + a_grip.depth.y;
        const bool onRight = mouse.x >= right - a_grip.depth.x;
        if (!onTop && !onRight)
        {
            return kNone;
        }
        if (mouse.y < top + a_grip.corner && mouse.x >= right - a_grip.corner)
        {
            return kTop | kRight;
        }
        return onTop ? kTop : kRight;
    }

    void ChatSize::Drag(const Grip& a_grip)
    {
        if (edges == kNone)
        {
            const auto hovered = FindEdges(a_grip);
            if (hovered == kNone)
            {
                return;
            }
            ShowResizeCursor(hovered);
            if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                return;
            }
            origin = Get(a_grip.maximum);
            size = origin;
            edges = hovered;
        }

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            Release();
            return;
        }
        ShowResizeCursor(edges);
        const auto delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0F);
        Extent moved = origin;
        if ((edges & kRight) != 0)
        {
            moved.width = Move(origin.width, delta.x / a_grip.step.x);
        }
        if ((edges & kTop) != 0)
        {
            moved.lines = Move(origin.lines, -delta.y / a_grip.step.y);
        }
        size = Limit(moved, a_grip.maximum);
    }

    void ChatSize::Release()
    {
        if (edges == kNone)
        {
            return;
        }
        edges = kNone;

        const auto live = Config::Get();
        if (size == Extent{ .width = live->ui.chatWidth, .lines = live->ui.chatLines })
        {
            return;
        }
        auto config = *live;
        config.ui.chatWidth = size.width;
        config.ui.chatLines = size.lines;
        const bool saved = Config::Apply(std::move(config));
        logs::info("Chat resized to {} font sizes by {} lines{}", size.width, size.lines, saved ? "" : "; not saved");
    }
}
