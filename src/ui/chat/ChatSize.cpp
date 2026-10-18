#include "ui/chat/ChatSize.h"
#include "config/PluginConfig.h"
#include "ui/chat/ChatPosition.h"

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
        const bool vertical = (a_edges & (kTop | kBottom)) != 0;
        const bool horizontal = (a_edges & (kLeft | kRight)) != 0;
        if (vertical && horizontal)
        {
            ImGui::SetMouseCursor(((a_edges & kTop) != 0) == ((a_edges & kRight) != 0) ? ImGuiMouseCursor_ResizeNESW
                                                                                         : ImGuiMouseCursor_ResizeNWSE);
            return;
        }
        ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);
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

    ImVec2 ChatSize::GetCorner(const ImVec2& a_corner) const
    {
        return ImVec2{ (edges & kLeft) != 0 ? corner.x : a_corner.x, (edges & kBottom) != 0 ? corner.y : a_corner.y };
    }

    ImVec2 ChatSize::GetRoom(const ImVec2& a_corner) const
    {
        const auto& display = ImGui::GetIO().DisplaySize;
        return ImVec2{ (edges & kLeft) != 0 ? fixed.x : display.x - a_corner.x,
            (edges & kBottom) != 0 ? display.y - fixed.y : a_corner.y };
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
        const auto min = ImGui::GetWindowPos();
        const ImVec2 max{ min.x + ImGui::GetWindowWidth(), min.y + ImGui::GetWindowHeight() };
        const auto find = [&](const ImVec2& a_depth) {
            std::uint8_t found = kNone;
            if (mouse.y < min.y + a_depth.y)
            {
                found |= kTop;
            }
            else if (mouse.y >= max.y - a_depth.y)
            {
                found |= kBottom;
            }
            if (mouse.x < min.x + a_depth.x)
            {
                found |= kLeft;
            }
            else if (mouse.x >= max.x - a_depth.x)
            {
                found |= kRight;
            }
            return found;
        };
        const auto strips = find(a_grip.depth);
        if (strips == kNone)
        {
            return kNone;
        }
        const auto corners = find(ImVec2{ a_grip.corner, a_grip.corner });
        const bool inCorner = (corners & (kTop | kBottom)) != 0 && (corners & (kLeft | kRight)) != 0;
        return inCorner ? corners : strips;
    }

    void ChatSize::Follow(const Units& a_units, const ImVec2& a_around)
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
        if ((edges & (kLeft | kRight)) != 0)
        {
            size.x = std::clamp(origin.x + ((edges & kLeft) != 0 ? -delta.x : delta.x), smallest.x, largest.x);
        }
        if ((edges & (kTop | kBottom)) != 0)
        {
            size.y = std::clamp(origin.y + ((edges & kTop) != 0 ? -delta.y : delta.y), smallest.y, largest.y);
        }
        settled = ToExtent(size, a_units);
        step = a_units.step;
        around = a_around;
        corner = Place(size);
    }

    ImVec2 ChatSize::Place(const ImVec2& a_size) const
    {
        // Dear ImGui truncates the window's size, so the edge held in place
        // stays there by the pixel.
        const ImVec2 frame{ std::floor(a_size.x + around.x), std::floor(a_size.y + around.y) };
        return ImVec2{ (edges & kLeft) != 0 ? fixed.x - frame.x : corner.x,
            (edges & kBottom) != 0 ? fixed.y + frame.y : corner.y };
    }

    bool ChatSize::Grab(const Grip& a_grip, const Units& a_units, const ImVec2& a_corner)
    {
        if (edges != kNone)
        {
            return true;
        }
        const auto hovered = FindEdges(a_grip);
        if (hovered == kNone)
        {
            return false;
        }
        ShowResizeCursor(hovered);
        if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            return true;
        }
        origin = Get(a_units);
        size = origin;
        settled = ToExtent(size, a_units);
        step = a_units.step;
        const auto position = ImGui::GetWindowPos();
        const auto frame = ImGui::GetWindowSize();
        around = ImVec2{ frame.x - size.x, frame.y - size.y };
        fixed = ImVec2{ position.x + frame.x, position.y };
        corner = a_corner;
        edges = hovered;
        return true;
    }

    void ChatSize::Release()
    {
        if (edges == kNone)
        {
            return;
        }
        const auto live = Config::Get();
        auto config = *live;
        config.ui.chatWidth = settled.width;
        config.ui.chatLines = settled.lines;
        if ((edges & (kLeft | kBottom)) != 0)
        {
            // Placed for the settled size, so the held edge stays where it is.
            ChatPosition::Store(config.ui, Place(ToPixels(settled, step)));
        }
        edges = kNone;
        if (config.ui == live->ui)
        {
            return;
        }
        const auto horizontal = config.ui.chatPositionHorizontal;
        const auto vertical = config.ui.chatPositionVertical;
        const bool saved = Config::Apply(std::move(config));
        logs::debug("Chat resized to {} font sizes by {} lines at {:.2f}% across and {:.2f}% up{}", settled.width,
            settled.lines, horizontal, vertical, saved ? "" : "; not saved");
    }
}
