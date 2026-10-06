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

        ImVec2 ToPixels(const ChatSize::Extent& a_size, const ImVec2& a_step)
        {
            return ImVec2{ static_cast<float>(a_size.width) * a_step.x, static_cast<float>(a_size.lines) * a_step.y };
        }

        ChatSize::Extent ToExtent(const ImVec2& a_size, const ChatSize::Units& a_units)
        {
            const auto count = [](float a_pixels, float a_step) {
                return static_cast<std::uint32_t>((std::max)(std::round(a_pixels / a_step), 0.0F));
            };
            return Limit(
                ChatSize::Extent{ .width = count(a_size.x, a_units.step.x), .lines = count(a_size.y, a_units.step.y) },
                a_units.maximum);
        }
    }

    void ChatSize::ShowResizeCursor(std::uint8_t a_edges)
    {
        ImGui::SetMouseCursor(a_edges == kTop ? ImGuiMouseCursor_ResizeNS
                : a_edges == kRight           ? ImGuiMouseCursor_ResizeEW
                                              : ImGuiMouseCursor_ResizeNESW);
    }

    ImVec2 ChatSize::Get(const Units& a_units) const
    {
        if (edges != kNone)
        {
            return size;
        }
        const auto config = Config::Get();
        return ToPixels(Limit(Extent{ .width = config->ui.chatWidth, .lines = config->ui.chatLines }, a_units.maximum),
            a_units.step);
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

    void ChatSize::Follow(const Units& a_units)
    {
        if (edges == kNone)
        {
            return;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            Release();
            return;
        }
        ShowResizeCursor(edges);
        const auto delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0F);
        const auto smallest = ToPixels(Limit(Extent{}, a_units.maximum), a_units.step);
        const auto largest
            = ToPixels(Limit(Extent{ .width = UISettings::kMaximumChatWidth, .lines = UISettings::kMaximumChatLines },
                           a_units.maximum),
                a_units.step);
        if ((edges & kRight) != 0)
        {
            size.x = std::clamp(origin.x + delta.x, smallest.x, largest.x);
        }
        if ((edges & kTop) != 0)
        {
            size.y = std::clamp(origin.y - delta.y, smallest.y, largest.y);
        }
        settled = ToExtent(size, a_units);
    }

    void ChatSize::Grab(const Grip& a_grip, const Units& a_units)
    {
        if (edges != kNone)
        {
            return;
        }
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
        origin = Get(a_units);
        size = origin;
        settled = ToExtent(size, a_units);
        edges = hovered;
    }

    void ChatSize::Release()
    {
        if (edges == kNone)
        {
            return;
        }
        edges = kNone;

        const auto live = Config::Get();
        if (settled == Extent{ .width = live->ui.chatWidth, .lines = live->ui.chatLines })
        {
            return;
        }
        auto config = *live;
        config.ui.chatWidth = settled.width;
        config.ui.chatLines = settled.lines;
        const bool saved = Config::Apply(std::move(config));
        logs::debug(
            "Chat resized to {} font sizes by {} lines{}", settled.width, settled.lines, saved ? "" : "; not saved");
    }
}
