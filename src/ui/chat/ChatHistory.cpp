#include "ui/chat/ChatHistory.h"
#include "sync/filters/PlayerFilters.h"
#include "text/Text.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"
#include "ui/chat/ChatStyle.h"
#include "ui/widgets/RoleBadge.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>
#include <span>
#include <utility>

namespace UI
{
    namespace
    {
        constexpr std::size_t kMaximumLinesPerChannel = 100;
        constexpr double kRecentLineSeconds = 10.0;
        constexpr double kRecentFadeSeconds = 2.0;
        constexpr float kWheelLines = 3.0F;
        constexpr float kLineSpacingFonts = 0.25F;
        constexpr float kFilterSpacingFonts = 0.375F;
        constexpr float kFilterDotFonts = 0.16F;

        struct Filter
        {
            Text::Id label;
            Multiplayer::ChatChannel channel;
            Text::Id hint;
        };

        constexpr std::array kFilters{
            Filter{ Text::Id::kChatGlobal, Multiplayer::ChatChannel::kGlobal, Text::Id::kChatGlobalFilterHint },
            Filter{ Text::Id::kChatNearby, Multiplayer::ChatChannel::kNearby, Text::Id::kChatNearbyFilterHint },
            Filter{ Text::Id::kChatWhispers, Multiplayer::ChatChannel::kWhisper, Text::Id::kChatWhispersFilterHint },
        };

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

        // Leaves the label as the last item and returns its width.
        float DrawLabel(const std::string& a_label, const ChatRoster::RoleMark& a_mark, std::size_t a_markOffset,
            const ImVec4& a_color)
        {
            ImGui::PushFont(Fonts::GetHeading(), 0.0F);
            if (!a_mark)
            {
                DrawColored(a_label, a_color);
                ImGui::PopFont();
                return ImGui::GetItemRectSize().x;
            }
            const char* const text = a_label.c_str();
            const char* const name = text + a_markOffset;
            ImGui::BeginGroup();
            ImGui::PushStyleColor(ImGuiCol_Text, a_color);
            if (a_markOffset != 0)
            {
                ImGui::TextUnformatted(text, name);
                ImGui::SameLine(0.0F, 0.0F);
            }
            RoleBadge::Mark(a_mark, a_color.w);
            ImGui::SameLine(0.0F, RoleBadge::GetMarkGap(ImGui::GetFontSize()));
            ImGui::TextUnformatted(name, text + a_label.size());
            ImGui::PopStyleColor();
            ImGui::EndGroup();
            ImGui::PopFont();
            return ImGui::GetItemRectSize().x;
        }

        const char* SkipSpaces(const char* a_text, const char* a_end)
        {
            return std::find_if(a_text, a_end, [](char a_character) { return a_character != ' '; });
        }

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

        // Calls a_piece(begin, end, mention or null) for the row's runs of plain text and of mentions, in order.
        template <class Piece>
        void ForEachPiece(const char* a_row, const char* a_rowEnd, const char* a_text,
            std::span<const ChatRoster::Mention> a_mentions, Piece&& a_piece)
        {
            const char* piece = a_row;
            for (const auto& mention : a_mentions)
            {
                if (a_text + mention.offset >= a_rowEnd)
                {
                    break;
                }
                const char* const begin = (std::max)(a_text + mention.offset, piece);
                const char* const end = (std::min)(a_text + mention.offset + mention.length, a_rowEnd);
                if (begin != piece)
                {
                    a_piece(piece, begin, nullptr);
                }
                a_piece(begin, end, &mention);
                piece = end;
            }
            if (piece != a_rowEnd)
            {
                a_piece(piece, a_rowEnd, nullptr);
            }
        }

        // The heading face, or the current font without it.
        ImFont* GetMentionFont()
        {
            auto* heading = Fonts::GetHeading();
            return heading != nullptr ? heading : ImGui::GetFont();
        }

        bool HasMention(const char* a_row, const char* a_rowEnd, const char* a_text,
            std::span<const ChatRoster::Mention> a_mentions)
        {
            return !a_mentions.empty() && a_text + a_mentions.front().offset < a_rowEnd
                && a_text + a_mentions.front().offset + a_mentions.front().length > a_row;
        }

        float MeasureMentionRow(const char* a_row, const char* a_rowEnd, const char* a_text,
            std::span<const ChatRoster::Mention> a_mentions)
        {
            auto* font = ImGui::GetFont();
            auto* mentionFont = GetMentionFont();
            const auto fontSize = ImGui::GetFontSize();
            float width = 0.0F;
            ForEachPiece(a_row, a_rowEnd, a_text, a_mentions,
                [&](const char* a_begin, const char* a_end, const ChatRoster::Mention* a_mention) {
                    width += (a_mention != nullptr ? mentionFont : font)
                                 ->CalcTextSizeA(fontSize, FLT_MAX, 0.0F, a_begin, a_end)
                                 .x;
                });
            return width;
        }

        // FindRowEnd measures in the text face; a row whose mentions in the heading face no
        // longer fit gives up words from its end. A single word stays.
        const char* FitMentionRow(const char* a_row, const char* a_rowEnd, const char* a_text,
            std::span<const ChatRoster::Mention> a_mentions, float a_width)
        {
            while (HasMention(a_row, a_rowEnd, a_text, a_mentions)
                && MeasureMentionRow(a_row, a_rowEnd, a_text, a_mentions) > a_width)
            {
                const char* last = a_rowEnd;
                while (last != a_row && last[-1] == ' ')
                {
                    --last;
                }
                while (last != a_row && last[-1] != ' ')
                {
                    --last;
                }
                if (last == a_row)
                {
                    break;
                }
                a_rowEnd = last;
            }
            return a_rowEnd;
        }

        // Draws the row as TextUnformatted would, with its mentions in the heading face.
        void DrawMentionRow(const char* a_row, const char* a_rowEnd, const char* a_text,
            std::span<const ChatRoster::Mention> a_mentions, const ImVec4& a_color)
        {
            ImGui::Dummy(ImVec2{ MeasureMentionRow(a_row, a_rowEnd, a_text, a_mentions), ImGui::GetTextLineHeight() });
            if (!ImGui::IsItemVisible())
            {
                return;
            }
            auto* font = ImGui::GetFont();
            auto* mentionFont = GetMentionFont();
            const auto fontSize = ImGui::GetFontSize();
            const auto min = ImGui::GetItemRectMin();
            auto* drawList = ImGui::GetWindowDrawList();
            const auto color = ImGui::GetColorU32(a_color);
            const auto accent = ImGui::GetColorU32(ChatStyle::WithAlpha(Theme::GetAccent(), a_color.w));
            auto x = min.x;
            ForEachPiece(a_row, a_rowEnd, a_text, a_mentions,
                [&](const char* a_begin, const char* a_end, const ChatRoster::Mention* a_mention) {
                    auto* pieceFont = a_mention != nullptr ? mentionFont : font;
                    const auto pieceColor = a_mention != nullptr && a_mention->local ? accent : color;
                    drawList->AddText(pieceFont, fontSize, ImVec2{ x, min.y }, pieceColor, a_begin, a_end);
                    x += pieceFont->CalcTextSizeA(fontSize, FLT_MAX, 0.0F, a_begin, a_end).x;
                });
        }

        void DrawAfterLabel(const std::string& a_text, std::span<const ChatRoster::Mention> a_mentions,
            const ImVec4& a_color, float a_labelWidth)
        {
            const char* const text = a_text.c_str();
            const char* row = text;
            const char* const end = row + a_text.size();
            const auto& style = ImGui::GetStyle();
            const auto full = ImGui::GetContentRegionAvail().x;
            auto width = full - a_labelWidth - style.ItemSpacing.x;

            ImGui::PushStyleColor(ImGuiCol_Text, a_color);
            while (row != end)
            {
                // Mentions before this row are behind it for good.
                while (!a_mentions.empty() && text + a_mentions.front().offset + a_mentions.front().length <= row)
                {
                    a_mentions = a_mentions.subspan(1);
                }
                auto rowEnd = FindRowEnd(row, end, width, full);
                if (HasMention(row, rowEnd, text, a_mentions))
                {
                    rowEnd = FitMentionRow(row, rowEnd, text, a_mentions, width);
                    // A word beside the label that no longer fits there starts the next row.
                    if (width < full && MeasureMentionRow(row, rowEnd, text, a_mentions) > width)
                    {
                        rowEnd = row;
                    }
                }
                if (rowEnd != row)
                {
                    if (width < full)
                    {
                        ImGui::SameLine();
                    }
                    else
                    {
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - style.ItemSpacing.y);
                    }
                    if (HasMention(row, rowEnd, text, a_mentions))
                    {
                        DrawMentionRow(row, rowEnd, text, a_mentions, a_color);
                    }
                    else
                    {
                        ImGui::TextUnformatted(row, rowEnd);
                    }
                    row = SkipSpaces(rowEnd, end);
                }
                width = full;
            }
            ImGui::PopStyleColor();
        }
    }

    void ChatHistory::Collect(double a_now, bool a_open)
    {
        if (const auto revision = PlayerFilters::GetRevision(); revision != filtersRevision)
        {
            filtersRevision = revision;
            for (auto& line : lines)
            {
                line.muted = line.sender != 0 && PlayerFilters::IsMuted(line.sender);
            }
            arrived = true;
        }
        // A role's name and color may change, and a deleted role's mark goes.
        if (const auto revision = ChatRoster::GetRevision(); revision != rosterRevision)
        {
            rosterRevision = revision;
            for (auto& line : lines)
            {
                if (line.role != 0)
                {
                    line.roleMark = ChatRoster::FindRoleMark(line.role);
                }
            }
        }

        ChatSync::DrainLines(incoming);
        for (auto& line : incoming)
        {
            const auto channel = static_cast<std::size_t>(line.channel);
            if (counts[channel] >= kMaximumLinesPerChannel)
            {
                Evict(line.channel);
            }
            ++counts[channel];

            const auto sender = !line.outgoing ? line.player.session : 0;
            const bool muted = sender != 0 && PlayerFilters::IsMuted(sender);
            const bool whisperable = !line.player.name.empty()
                && (line.channel == Multiplayer::ChatChannel::kWhisper || !line.outgoing);
            const auto role = line.player.role;
            lines.push_back(Line{ .channel = line.channel,
                .label = std::format("{}:", line.sender),
                .markOffset = line.nameOffset,
                .text = std::move(line.text),
                .mentions = std::move(line.mentions),
                .player = whisperable ? std::move(line.player) : Multiplayer::PlayerIdentity{},
                .role = role,
                .roleMark = ChatRoster::FindRoleMark(role),
                .sender = sender,
                .muted = muted,
                .receivedAt = a_now });
            if (line.outgoing)
            {
                SetShown(line.channel, true);
            }
            if (!muted && !(a_open && Shows(line.channel)))
            {
                unread[static_cast<std::size_t>(line.channel)] = true;
            }
            arrived = true;
        }
        incoming.clear();
    }

    void ChatHistory::Evict(Multiplayer::ChatChannel a_channel)
    {
        const auto oldest = std::ranges::find(lines, a_channel, &Line::channel);
        if (oldest != lines.end())
        {
            lines.erase(oldest);
            --counts[static_cast<std::size_t>(a_channel)];
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
        return Shows(a_line.channel) && !a_line.muted;
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
            const auto* const label = Text::GetCString(filter.label);
            const auto channel = static_cast<std::size_t>(filter.channel);
            if (index > 0)
            {
                ImGui::SameLine(0.0F, std::floor(kFilterSpacingFonts * fontSize));
            }
            const auto textSize = ImGui::CalcTextSize(label);
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
            ImGui::SetItemTooltip("%s", Text::GetCString(filter.hint));
            if (on)
            {
                unread[channel] = false;
            }

            const auto min = ImGui::GetItemRectMin();
            const auto max = ImGui::GetItemRectMax();
            const auto& channelColor = ChatStyle::GetChannelColor(filter.channel);
            const auto& color = on ? channelColor : style.Colors[hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled];
            const auto fillAlpha = on ? (hovered ? ChatStyle::kHoveredChipFillAlpha : ChatStyle::kChipFillAlpha)
                                      : (hovered ? ChatStyle::kHoveredOffChipFillAlpha : 0.0F);
            ChatStyle::DrawChipFrame(color, fillAlpha);
            drawList->AddText(ImVec2{ min.x + padding.x, min.y + padding.y }, ImGui::GetColorU32(color), label);
            if (!on && unread[channel])
            {
                const auto radius = fontSize * kFilterDotFonts;
                drawList->AddCircleFilled(
                    ImVec2{ max.x - radius * 0.5F, min.y + radius * 0.5F }, radius, ImGui::GetColorU32(channelColor));
            }
        }
    }

    std::optional<Multiplayer::PlayerIdentity> ChatHistory::DrawLines(
        const WindowInput& a_input, ImVec2 a_size, bool a_closeMenu, bool a_wheelElsewhere, bool a_pageKeys)
    {
        std::optional<Multiplayer::PlayerIdentity> picked;
        if (rewrappedHeight >= 0.0F)
        {
            ImGui::SetNextWindowContentSize(ImVec2{ 0.0F, rewrappedHeight });
            rewrappedHeight = -1.0F;
        }
        if (ImGui::BeginChild("##ChatHistory", a_size))
        {
            const auto size = ImGui::GetWindowSize();
            const bool resized = size.x != shownSize.x || size.y != shownSize.y;
            shownSize = size;
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
                    const auto labelWidth = DrawLabel(
                        line.label, line.roleMark, line.markOffset, ChatStyle::GetChannelColor(line.channel));
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
                    DrawAfterLabel(line.text, line.mentions, textColor, labelWidth);
                    ImGui::EndGroup();
                    if (line.player.playerID != 0 && ImGui::IsItemHovered(kHover)
                        && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
                    {
                        playerMenu.Open(line.player);
                        ImGui::OpenPopup("##ChatLineMenu");
                    }
                }
                ImGui::PopStyleVar();
            };

            // Dear ImGui scrolls by what the lines took a frame ago, so lines that wrap
            // at another width would leave the latest line off the bottom for a frame.
            // After a rewrap they are first laid out clipped, which draws nothing and
            // takes no click, to find what they take now.
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
                if (playerMenu.Draw())
                {
                    picked = playerMenu.GetPlayer();
                }
                ImGui::EndPopup();
            }

            // The child scrolls by itself under the cursor.
            const auto lineHeight = GetLinePitch();
            const auto wheel = a_input.wheelSteps
                + (ImGui::IsWindowHovered() || a_wheelElsewhere ? 0.0F : ImGui::GetIO().MouseWheel);
            auto scroll = ImGui::GetScrollY() - wheel * kWheelLines * lineHeight;
            if (a_pageKeys && ImGui::IsKeyPressed(ImGuiKey_PageUp))
            {
                scroll -= size.y - lineHeight;
            }
            if (a_pageKeys && ImGui::IsKeyPressed(ImGuiKey_PageDown))
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
        for (auto index = recentCount; index-- > 0;)
        {
            const auto& line = *recent[index];
            const auto alpha = GetRecentAlpha(line.receivedAt, a_now);
            const auto labelWidth = DrawLabel(line.label, line.roleMark, line.markOffset,
                ChatStyle::WithAlpha(ChatStyle::GetChannelColor(line.channel), alpha));
            DrawAfterLabel(line.text, line.mentions, ChatStyle::WithAlpha(textColor, alpha), labelWidth);
        }
        ImGui::PopStyleVar();
    }
}
