#include "ui/chat/ChatHistory.h"
#include "ui/Fonts.h"
#include "ui/chat/ChatStyle.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace UI
{
    namespace
    {
        constexpr std::size_t kMaximumLinesPerKind = 100;
        constexpr std::size_t kNoticeKind = 3;
        constexpr double kRecentLineSeconds = 10.0;
        constexpr double kRecentFadeSeconds = 2.0;
        constexpr float kWheelLines = 3.0F;
        // In font sizes. Lines stand closer than the theme's items do.
        constexpr float kLineSpacingFonts = 0.25F;
        // Between filters, and the unread dot on a filter's corner.
        constexpr float kFilterSpacingFonts = 0.375F;
        constexpr float kFilterDotFonts = 0.16F;

        struct Filter
        {
            const char* label;
            Multiplayer::ChatChannel channel;
        };

        constexpr std::array kFilters{
            Filter{ "Global", Multiplayer::ChatChannel::kGlobal },
            Filter{ "Local", Multiplayer::ChatChannel::kLocal },
            Filter{ "Whispers", Multiplayer::ChatChannel::kWhisper },
        };

        std::size_t GetKind(ChatSync::LineKind a_kind, Multiplayer::ChatChannel a_channel)
        {
            return a_kind == ChatSync::LineKind::kNotice ? kNoticeKind : static_cast<std::size_t>(a_channel);
        }

        float GetRecentAlpha(double a_receivedAt, double a_now)
        {
            const auto remaining = kRecentLineSeconds - (a_now - a_receivedAt);
            return static_cast<float>(std::clamp(remaining / kRecentFadeSeconds, 0.0, 1.0));
        }

        float GetLineSpacing()
        {
            return std::floor(kLineSpacingFonts * ImGui::GetFontSize());
        }

        // Not DrawText, which windows.h makes a macro.
        void DrawColored(const std::string& a_text, const ImVec4& a_color)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, a_color);
            ImGui::TextUnformatted(a_text.c_str(), a_text.c_str() + a_text.size());
            ImGui::PopStyleColor();
        }

        void DrawWrapped(const std::string& a_text, const ImVec4& a_color)
        {
            ImGui::PushTextWrapPos(0.0F);
            DrawColored(a_text, a_color);
            ImGui::PopTextWrapPos();
        }

        // Who a line is from, in the heading face.
        void DrawLabel(const std::string& a_label, const ImVec4& a_color)
        {
            ImGui::PushFont(Fonts::GetHeading(), 0.0F);
            DrawColored(a_label, a_color);
            ImGui::PopFont();
        }

        const char* SkipSpaces(const char* a_text, const char* a_end)
        {
            return std::find_if(a_text, a_end, [](char a_character) { return a_character != ' '; });
        }

        // Where a row of a_width that starts at a_row ends, with a_full the
        // width of a whole row. Words stay whole, except one wider than a
        // whole row: no row holds it, so it fills this one and is cut at its
        // end. A row beside a label, narrower than a whole one, is left empty
        // by a word that only fits a whole row; any other row takes text.
        const char* FindRowEnd(const char* a_row, const char* a_end, float a_width, float a_full)
        {
            const bool besideLabel = a_width < a_full;
            if (besideLabel && a_width <= 0.0F)
            {
                return a_row;
            }
            auto* font = ImGui::GetFont();
            const auto fontSize = ImGui::GetFontSize();
            const char* wrapped = font->CalcWordWrapPosition(fontSize, a_row, a_end, a_width);
            if (wrapped == a_end)
            {
                return a_end;
            }

            // The word the row ends before, or inside: Dear ImGui also breaks
            // after punctuation and cuts what the row is too narrow for.
            const char* word = wrapped;
            if (*word == ' ')
            {
                word = SkipSpaces(word, a_end);
            }
            else
            {
                while (word != a_row && word[-1] != ' ')
                {
                    --word;
                }
            }
            if (ImGui::CalcTextSize(word, std::find(word, a_end, ' ')).x <= a_full)
            {
                return *wrapped == ' ' || (word == a_row && !besideLabel) ? wrapped : word;
            }
            const char* cut = a_row;
            font->CalcTextSizeA(fontSize, a_width, 0.0F, a_row, a_end, &cut);
            if (cut == a_row)
            {
                return besideLabel ? a_row : wrapped;
            }
            return (std::max)(cut, wrapped);
        }

        // A line's text after its label, the item before it: on the label's
        // row as far as it fits, then from the left edge under the label.
        void DrawAfterLabel(const std::string& a_text, const ImVec4& a_color)
        {
            const char* row = a_text.c_str();
            const char* const end = row + a_text.size();
            const auto& style = ImGui::GetStyle();
            const auto full = ImGui::GetContentRegionAvail().x;
            auto width = full - ImGui::GetItemRectSize().x - style.ItemSpacing.x;

            ImGui::PushStyleColor(ImGuiCol_Text, a_color);
            while (row != end)
            {
                const char* const rowEnd = FindRowEnd(row, end, width, full);
                if (rowEnd != row)
                {
                    if (width < full)
                    {
                        ImGui::SameLine();
                    }
                    else
                    {
                        // As close under the row above as wrapped rows are.
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - style.ItemSpacing.y);
                    }
                    ImGui::TextUnformatted(row, rowEnd);
                    row = SkipSpaces(rowEnd, end);
                }
                width = full;
            }
            ImGui::PopStyleColor();
        }
    }

    void ChatHistory::Collect(double a_now, bool a_open)
    {
        ChatSync::DrainLines(incoming);
        for (auto& line : incoming)
        {
            const auto kind = GetKind(line.kind, line.channel);
            if (counts[kind] >= kMaximumLinesPerKind)
            {
                Evict(kind);
            }
            ++counts[kind];

            const bool fromPlayer = line.kind == ChatSync::LineKind::kPlayer;
            // Whispering oneself by a click on one's own line would only surprise.
            const bool whisperable = fromPlayer && !line.player.name.empty()
                && (line.channel == Multiplayer::ChatChannel::kWhisper || !line.outgoing);
            lines.push_back(Line{ .kind = line.kind,
                .channel = line.channel,
                .label = fromPlayer ? std::move(line.sender) + ":" : std::string{},
                .text = std::move(line.text),
                .player = whisperable ? std::move(line.player) : Multiplayer::RosterEntry{},
                .receivedAt = a_now });
            // The player's own line stays in view.
            if (fromPlayer && line.outgoing)
            {
                SetShown(line.channel, true);
            }
            if (fromPlayer && !(a_open && Shows(line.channel)))
            {
                unread[static_cast<std::size_t>(line.channel)] = true;
            }
            arrived = true;
        }
        incoming.clear();
    }

    void ChatHistory::Evict(std::size_t a_kind)
    {
        const auto oldest = std::ranges::find_if(
            lines, [a_kind](const Line& a_line) { return GetKind(a_line.kind, a_line.channel) == a_kind; });
        if (oldest != lines.end())
        {
            lines.erase(oldest);
            --counts[a_kind];
        }
    }

    void ChatHistory::SetShown(Multiplayer::ChatChannel a_channel, bool a_shown)
    {
        auto& current = shown[static_cast<std::size_t>(a_channel)];
        if (current != a_shown)
        {
            current = a_shown;
            scrollToLatest = true;
        }
    }

    bool ChatHistory::Shows(Multiplayer::ChatChannel a_channel) const
    {
        return shown[static_cast<std::size_t>(a_channel)];
    }

    bool ChatHistory::Shows(const Line& a_line) const
    {
        return a_line.kind == ChatSync::LineKind::kNotice || Shows(a_line.channel);
    }

    float ChatHistory::GetLinePitch()
    {
        return ImGui::GetTextLineHeight() + GetLineSpacing();
    }

    void ChatHistory::DrawFilters()
    {
        const auto& style = ImGui::GetStyle();
        const auto fontSize = ImGui::GetFontSize();
        const auto padding = ChatStyle::GetChipPadding();
        auto* drawList = ImGui::GetWindowDrawList();

        for (std::size_t index = 0; index < kFilters.size(); ++index)
        {
            const auto& filter = kFilters[index];
            const auto channel = static_cast<std::size_t>(filter.channel);
            if (index > 0)
            {
                ImGui::SameLine(0.0F, std::floor(kFilterSpacingFonts * fontSize));
            }
            const auto textSize = ImGui::CalcTextSize(filter.label);
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::InvisibleButton(
                    "##ChatFilter", ImVec2{ textSize.x + padding.x * 2.0F, textSize.y + padding.y * 2.0F }))
            {
                SetShown(filter.channel, !shown[channel]);
            }
            ImGui::PopID();
            const bool hovered = ImGui::IsItemHovered();
            if (hovered)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            const bool on = shown[channel];
            if (on)
            {
                unread[channel] = false;
            }

            // On, a chip filled with its channel's color; off, a muted outline.
            const auto min = ImGui::GetItemRectMin();
            const auto max = ImGui::GetItemRectMax();
            const auto& channelColor = ChatStyle::GetChannelColor(filter.channel);
            const auto& color = on ? channelColor : style.Colors[hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled];
            const auto fillAlpha = on ? (hovered ? ChatStyle::kHoveredChipFillAlpha : ChatStyle::kChipFillAlpha)
                                      : (hovered ? ChatStyle::kHoveredOffChipFillAlpha : 0.0F);
            ChatStyle::DrawChipFrame(color, fillAlpha);
            drawList->AddText(ImVec2{ min.x + padding.x, min.y + padding.y }, ImGui::GetColorU32(color), filter.label);
            if (!on && unread[channel])
            {
                const auto radius = fontSize * kFilterDotFonts;
                drawList->AddCircleFilled(
                    ImVec2{ max.x - radius * 0.5F, min.y + radius * 0.5F }, radius, ImGui::GetColorU32(channelColor));
            }
        }
    }

    std::optional<Multiplayer::RosterEntry> ChatHistory::DrawLines(
        const WindowInput& a_input, ImVec2 a_size, bool a_closeMenu, bool a_wheelElsewhere)
    {
        std::optional<Multiplayer::RosterEntry> picked;
        if (rewrappedHeight >= 0.0F)
        {
            ImGui::SetNextWindowContentSize(ImVec2{ 0.0F, rewrappedHeight });
            rewrappedHeight = -1.0F;
        }
        if (ImGui::BeginChild("##ChatHistory", a_size))
        {
            // Before this frame's lines extend it: whether the latest was in
            // view. A resized chat has another scroll range and rows that wrap
            // elsewhere, so it keeps what it had and follows again below.
            const auto size = ImGui::GetWindowSize();
            const bool resized = size.x != shownSize.x || size.y != shownSize.y;
            shownSize = size;
            // Less the scrollbar while it shows.
            const auto width = ImGui::GetContentRegionAvail().x;
            const bool rewrapped = width != shownWidth;
            shownWidth = width;
            if (!resized)
            {
                following = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0F;
            }
            // The input is active while the player types, which would
            // otherwise keep every other item from being hovered.
            constexpr auto kHover = ImGuiHoveredFlags_AllowWhenBlockedByActiveItem;
            auto* drawList = ImGui::GetWindowDrawList();
            const auto& textColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);

            const auto lineSpacing = GetLineSpacing();
            const auto drawLines = [&] {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{ ImGui::GetStyle().ItemSpacing.x, lineSpacing });
                for (const auto& line : lines)
                {
                    if (!Shows(line))
                    {
                        continue;
                    }
                    ImGui::BeginGroup();
                    if (line.kind == ChatSync::LineKind::kPlayer)
                    {
                        DrawLabel(line.label, ChatStyle::GetChannelColor(line.channel));
                        if (line.player.playerID != 0 && ImGui::IsItemHovered(kHover))
                        {
                            const auto min = ImGui::GetItemRectMin();
                            const auto max = ImGui::GetItemRectMax();
                            drawList->AddRectFilled(ImVec2{ min.x, max.y - 1.0F }, max,
                                ImGui::GetColorU32(ChatStyle::GetChannelColor(line.channel)));
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                            {
                                picked = line.player;
                            }
                        }
                        DrawAfterLabel(line.text, textColor);
                    }
                    else
                    {
                        DrawWrapped(line.text, ChatStyle::kNoticeColor);
                    }
                    ImGui::EndGroup();
                    if (ImGui::IsItemHovered(kHover) && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
                    {
                        menuLine = line;
                        ImGui::OpenPopup("##ChatLineMenu");
                    }
                }
                ImGui::PopStyleVar();
            };

            // Dear ImGui scrolls by what the lines took a frame ago. When they
            // wrap at another width, rows above the view change too, which
            // would leave the latest line off the bottom for a frame, each
            // frame of a drag. There the lines are first laid out clipped,
            // which draws nothing and takes no click, to find what they take
            // now, and then drawn where that scrolls them to. A new height
            // needs none of it: the scroll set from the end of the lines is
            // applied at the height of the frame it lands in.
            float latestScroll = -1.0F;
            if (rewrapped && following)
            {
                const auto top = ImGui::GetCursorPosY();
                ImGui::PushClipRect(ImVec2{}, ImVec2{}, false);
                drawLines();
                ImGui::PopClipRect();
                rewrappedHeight = (std::max)(ImGui::GetCursorPosY() - top - lineSpacing, 0.0F);
                latestScroll = (std::max)(rewrappedHeight - size.y, 0.0F);
                ImGui::SetCursorPosY(top + ImGui::GetScrollY() - latestScroll);
            }
            drawLines();

            menuOpen = ImGui::BeginPopup("##ChatLineMenu");
            if (menuOpen)
            {
                if (a_closeMenu)
                {
                    ImGui::CloseCurrentPopup();
                }
                if (menuLine.player.playerID != 0)
                {
                    if (ImGui::MenuItem("Whisper"))
                    {
                        picked = menuLine.player;
                    }
                    if (ImGui::MenuItem("Copy name"))
                    {
                        ImGui::SetClipboardText(menuLine.player.name.c_str());
                    }
                }
                if (ImGui::MenuItem("Copy message"))
                {
                    ImGui::SetClipboardText(menuLine.text.c_str());
                }
                ImGui::EndPopup();
            }

            // The child scrolls by itself under the cursor; elsewhere, and
            // while the cursor is hidden, the wheel still scrolls the lines.
            const auto lineHeight = GetLinePitch();
            const auto wheel = a_input.wheelSteps
                + (ImGui::IsWindowHovered() || a_wheelElsewhere ? 0.0F : ImGui::GetIO().MouseWheel);
            auto scroll = ImGui::GetScrollY() - wheel * kWheelLines * lineHeight;
            if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
            {
                scroll -= size.y - lineHeight;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_PageDown))
            {
                scroll += size.y - lineHeight;
            }
            if (scroll != ImGui::GetScrollY())
            {
                ImGui::SetScrollY(std::clamp(scroll, 0.0F, ImGui::GetScrollMaxY()));
            }
            else if (latestScroll >= 0.0F)
            {
                ImGui::SetScrollY(latestScroll);
            }
            else if (scrollToLatest || ((arrived || resized) && following))
            {
                ImGui::SetScrollHereY(1.0F);
            }
            scrollToLatest = false;
            arrived = false;
        }
        ImGui::EndChild();
        return picked;
    }

    float ChatHistory::FindRecent(double a_now)
    {
        recentCount = 0;
        for (auto line = lines.rbegin(); line != lines.rend() && recentCount < recent.size(); ++line)
        {
            if (a_now - line->receivedAt >= kRecentLineSeconds)
            {
                break;
            }
            if (Shows(*line))
            {
                recent[recentCount++] = &*line;
            }
        }
        return recentCount == 0 ? 0.0F : GetRecentAlpha(recent[0]->receivedAt, a_now);
    }

    void ChatHistory::DrawRecent(double a_now) const
    {
        const auto& textColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{ ImGui::GetStyle().ItemSpacing.x, GetLineSpacing() });
        // Found newest first.
        for (auto index = recentCount; index-- > 0;)
        {
            const auto& line = *recent[index];
            const auto alpha = GetRecentAlpha(line.receivedAt, a_now);
            if (line.kind == ChatSync::LineKind::kPlayer)
            {
                DrawLabel(line.label, ChatStyle::WithAlpha(ChatStyle::GetChannelColor(line.channel), alpha));
                DrawAfterLabel(line.text, ChatStyle::WithAlpha(textColor, alpha));
            }
            else
            {
                DrawWrapped(line.text, ChatStyle::WithAlpha(ChatStyle::kNoticeColor, alpha));
            }
        }
        ImGui::PopStyleVar();
    }
}
