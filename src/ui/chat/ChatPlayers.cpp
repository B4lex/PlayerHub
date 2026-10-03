#include "ui/chat/ChatPlayers.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"
#include "ui/chat/ChatHistory.h"
#include "ui/chat/ChatStyle.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>
#include <numbers>

namespace UI
{
    namespace
    {
        // In font sizes: the list's content, and the gap between the person
        // drawn on the button and its number.
        constexpr float kListWidthFonts = 13.0F;
        constexpr float kIconGapFonts = 0.3F;
        // Of the person's height, which is the digits'.
        constexpr float kIconWidthOfHeight = 0.9F;
        // In font sizes: the dot that marks a player nearby.
        constexpr float kNearbyDotFonts = 0.25F;
        constexpr double kNearbySeconds = 0.5;

        // The rows of the line a digit takes, from its top: what the person
        // beside the number fills, rather than the whole line with the room
        // above the digits and below the baseline.
        struct DigitRows
        {
            float top{ 0.0F };
            float height{ 0.0F };
        };

        DigitRows FindDigitRows()
        {
            if (const auto* digit = ImGui::GetFontBaked()->FindGlyphNoFallback('0'))
            {
                return DigitRows{ .top = std::round(digit->Y0), .height = std::round(digit->Y1 - digit->Y0) };
            }
            return DigitRows{ .top = 0.0F, .height = ImGui::GetFontSize() };
        }

        // A head over shoulders in a box of a_size, so it does not depend on
        // a font having the glyph.
        void DrawPersonIcon(ImDrawList& a_list, const ImVec2& a_min, const ImVec2& a_size, ImU32 a_color)
        {
            const auto middle = a_min.x + a_size.x * 0.5F;
            const auto head = a_size.y * 0.24F;
            a_list.AddCircleFilled(ImVec2{ middle, a_min.y + head }, head, a_color);
            a_list.PathArcTo(ImVec2{ middle, a_min.y + a_size.y }, a_size.x * 0.5F, std::numbers::pi_v<float>,
                std::numbers::pi_v<float> * 2.0F);
            a_list.PathFillConvex(a_color);
        }

        // Tab would move the keyboard to the rows; the field keeps it.
        int KeepTab(ImGuiInputTextCallbackData*)
        {
            return 0;
        }

        void DrawMuted(const char* a_text)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", a_text);
            ImGui::PopStyleColor();
        }
    }

    float ChatPlayers::GetWidth()
    {
        return std::floor(kListWidthFonts * ImGui::GetFontSize());
    }

    bool ChatPlayers::IsHovered() const
    {
        return open && ImGui::IsMouseHoveringRect(shownMin, shownMax, false);
    }

    void ChatPlayers::Close()
    {
        open = false;
        search[0] = '\0';
        foldedSearch.clear();
        Match();
    }

    void ChatPlayers::DrawButton()
    {
        const auto count = ChatRoster::GetPlayerCount();
        if (count != labelCount)
        {
            labelCount = count;
            const auto written = std::format_to_n(label.data(), label.size() - 1, "{}", count);
            *written.out = '\0';
        }
        const bool online = count > 0;
        const char* text = online ? label.data() : "Offline";

        const auto& style = ImGui::GetStyle();
        const auto fontSize = ImGui::GetFontSize();
        const auto padding = ChatStyle::GetChipPadding();
        const auto textSize = ImGui::CalcTextSize(text);
        const auto digit = FindDigitRows();
        const auto icon = online ? std::round(kIconWidthOfHeight * digit.height) : 0.0F;
        const auto gap = online ? std::floor(kIconGapFonts * fontSize) : 0.0F;
        const ImVec2 size{ padding.x * 2.0F + icon + gap + textSize.x, textSize.y + padding.y * 2.0F };

        ImGui::SameLine();
        if (const auto room = ImGui::GetContentRegionAvail().x - size.x; room > 0.0F)
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room);
        }
        if (ImGui::InvisibleButton("##ChatPlayers", size))
        {
            if (open)
            {
                Close();
            }
            else
            {
                open = true;
            }
        }
        const bool hovered = ImGui::IsItemHovered();
        if (hovered)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }

        // Open, a chip in the accent, as a filter that is on is in its
        // channel's color; closed, an outline in the text color.
        const auto& color = open ? Theme::kAccent : style.Colors[online ? ImGuiCol_Text : ImGuiCol_TextDisabled];
        const auto fillAlpha = open ? (hovered ? ChatStyle::kHoveredChipFillAlpha : ChatStyle::kChipFillAlpha)
                                    : (hovered ? ChatStyle::kHoveredOffChipFillAlpha : 0.0F);
        ChatStyle::DrawChipFrame(color, fillAlpha);
        const auto min = ImGui::GetItemRectMin();
        auto* drawList = ImGui::GetWindowDrawList();
        const ImVec2 content{ min.x + padding.x, min.y + padding.y };
        if (online)
        {
            DrawPersonIcon(*drawList, ImVec2{ content.x, content.y + digit.top }, ImVec2{ icon, digit.height },
                ImGui::GetColorU32(color));
        }
        drawList->AddText(ImVec2{ content.x + icon + gap, content.y }, ImGui::GetColorU32(color), text);
    }

    void ChatPlayers::Refresh(double a_now)
    {
        if (ChatRoster::CopyPlayersIfChanged(revision, players, localPlayer))
        {
            Match();
        }
        if (a_now - nearbyAt >= kNearbySeconds)
        {
            nearbyAt = a_now;
            nearby.clear();
            ChatRoster::CopyNearbyPlayers(nearby);
        }
    }

    void ChatPlayers::Match()
    {
        matches.clear();
        std::optional<std::uint32_t> local;
        for (std::uint32_t index = 0; index < players.size(); ++index)
        {
            const auto& entry = players[index];
            if (!foldedSearch.empty() && entry.folded.find(foldedSearch) == std::string::npos)
            {
                continue;
            }
            if (entry.player.playerID == localPlayer)
            {
                local = index;
            }
            else
            {
                matches.push_back(index);
            }
        }
        if (local)
        {
            matches.push_back(*local);
        }
    }

    std::optional<Multiplayer::RosterEntry> ChatPlayers::DrawList(float a_ruleGap, bool a_closeMenu)
    {
        Refresh(ImGui::GetTime());

        // As high as the chat's filters, so the rules under both line up.
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ChatStyle::GetChipPadding());
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##ChatPlayerSearch", "Search", search.data(), search.size(),
                ImGuiInputTextFlags_CallbackCompletion, &KeepTab))
        {
            foldedSearch = ChatRoster::FoldName(search.data());
            Match();
        }
        ImGui::PopStyleVar();
        ChatStyle::DrawRule(a_ruleGap);

        std::optional<Multiplayer::RosterEntry> picked;
        DrawRows(picked, a_closeMenu);

        shownMin = ImGui::GetWindowPos();
        const auto size = ImGui::GetWindowSize();
        shownMax = ImVec2{ shownMin.x + size.x, shownMin.y + size.y };
        return picked;
    }

    void ChatPlayers::DrawRows(std::optional<Multiplayer::RosterEntry>& a_picked, bool a_closeMenu)
    {
        if (ImGui::BeginChild("##ChatPlayerRows"))
        {
            if (players.empty())
            {
                DrawMuted("Not connected to the server.");
            }
            else if (matches.empty())
            {
                DrawMuted("No player with that name is online");
            }

            // The chat's field is active while the player types, which would
            // otherwise keep the rows from being hovered.
            constexpr auto kHover = ImGuiHoveredFlags_AllowWhenBlockedByActiveItem;
            const auto& style = ImGui::GetStyle();
            const auto& mutedColor = style.Colors[ImGuiCol_TextDisabled];
            const auto lineHeight = ImGui::GetTextLineHeight();
            const auto pitch = ChatHistory::GetLinePitch();
            const auto dotRadius = std::floor(kNearbyDotFonts * ImGui::GetFontSize());
            const auto dotSpace = dotRadius * 2.0F + style.ItemSpacing.x;
            auto* drawList = ImGui::GetWindowDrawList();

            // Rows a chat line apart, so the clipper can skip the ones out of view.
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{ style.ItemSpacing.x, pitch - lineHeight });
            ImGui::PushFont(Fonts::GetHeading(), 0.0F);
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(matches.size()), pitch);
            while (clipper.Step())
            {
                for (auto row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    const auto& player = players[matches[static_cast<std::size_t>(row)]].player;
                    const bool self = player.playerID == localPlayer;
                    ImGui::PushID(static_cast<int>(player.playerID));
                    const bool clicked = ImGui::InvisibleButton(
                        "##ChatPlayer", ImVec2{ (std::max)(ImGui::GetContentRegionAvail().x, 1.0F), lineHeight });
                    ImGui::PopID();
                    const bool hovered = !self && ImGui::IsItemHovered(kHover);
                    const auto min = ImGui::GetItemRectMin();
                    const auto max = ImGui::GetItemRectMax();
                    const char* name = player.name.c_str();
                    const char* nameEnd = name + player.name.size();
                    const ImVec4 nameClip{ min.x, min.y, max.x - dotSpace, max.y };

                    if (self)
                    {
                        drawList->AddText(
                            nullptr, 0.0F, min, ImGui::GetColorU32(mutedColor), name, nameEnd, 0.0F, &nameClip);
                        const auto nameWidth = ImGui::CalcTextSize(name, nameEnd).x;
                        drawList->AddText(nullptr, 0.0F, ImVec2{ min.x + nameWidth, min.y },
                            ImGui::GetColorU32(mutedColor), " (you)", nullptr, 0.0F, &nameClip);
                        continue;
                    }

                    const auto nameColor = ImGui::GetColorU32(ChatStyle::kWhisperColor);
                    drawList->AddText(nullptr, 0.0F, min, nameColor, name, nameEnd, 0.0F, &nameClip);
                    if (std::ranges::binary_search(nearby, player.playerID))
                    {
                        const ImVec2 center{ max.x - dotRadius, std::floor((min.y + max.y) * 0.5F) };
                        drawList->AddCircleFilled(center, dotRadius, ImGui::GetColorU32(ChatStyle::kLocalColor));
                        if (hovered && ImGui::IsMouseHoveringRect(ImVec2{ max.x - dotSpace, min.y }, max))
                        {
                            ImGui::PopFont();
                            ImGui::SetTooltip("Near you");
                            ImGui::PushFont(Fonts::GetHeading(), 0.0F);
                        }
                    }
                    if (!hovered)
                    {
                        continue;
                    }
                    const auto underline = (std::min)(min.x + ImGui::CalcTextSize(name, nameEnd).x, nameClip.z);
                    drawList->AddRectFilled(ImVec2{ min.x, max.y - 1.0F }, ImVec2{ underline, max.y }, nameColor);
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (clicked)
                    {
                        a_picked = player;
                    }
                    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right))
                    {
                        menuPlayer = player;
                        ImGui::OpenPopup("##ChatPlayerMenu");
                    }
                }
            }
            ImGui::PopFont();
            ImGui::PopStyleVar();

            menuOpen = ImGui::BeginPopup("##ChatPlayerMenu");
            if (menuOpen)
            {
                if (a_closeMenu)
                {
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::MenuItem("Whisper"))
                {
                    a_picked = menuPlayer;
                }
                if (ImGui::MenuItem("Copy name"))
                {
                    ImGui::SetClipboardText(menuPlayer.name.c_str());
                }
                ImGui::EndPopup();
            }
        }
        ImGui::EndChild();
    }
}
