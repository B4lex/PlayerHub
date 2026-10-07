#include "ui/chat/ChatPlayers.h"
#include "sync/filters/PlayerFilters.h"
#include "ui/Theme.h"
#include "ui/chat/ChatHistory.h"
#include "ui/chat/ChatStyle.h"
#include "ui/widgets/Controls.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>
#include <numbers>

namespace UI
{
    namespace
    {
        constexpr float kListWidthFonts = 13.0F;
        constexpr float kIconGapFonts = 0.3F;
        constexpr float kIconWidthOfHeight = 0.9F;
        constexpr float kFilterMarkFonts = 0.75F;
        constexpr double kNearbySeconds = 0.5;
        constexpr ImVec4 kDistantPlayerColor{ 0.76F, 0.75F, 0.73F, 1.0F };

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

        void DrawPersonIcon(ImDrawList& a_list, const ImVec2& a_min, const ImVec2& a_size, ImU32 a_color)
        {
            const auto middle = a_min.x + a_size.x * 0.5F;
            const auto head = a_size.y * 0.24F;
            a_list.AddCircleFilled(ImVec2{ middle, a_min.y + head }, head, a_color);
            a_list.PathArcTo(ImVec2{ middle, a_min.y + a_size.y }, a_size.x * 0.5F, std::numbers::pi_v<float>,
                std::numbers::pi_v<float> * 2.0F);
            a_list.PathFillConvex(a_color);
        }

        void DrawMutedMark(ImDrawList& a_list, const ImVec2& a_min, float a_size, ImU32 a_color, float a_thickness)
        {
            const ImVec2 bubbleMax{ a_min.x + a_size, a_min.y + a_size * 0.7F };
            a_list.AddRect(ImVec2{ a_min.x, a_min.y + a_size * 0.05F }, bubbleMax, a_color, a_size * 0.2F,
                ImDrawFlags_None, a_thickness);
            a_list.AddTriangleFilled(ImVec2{ a_min.x + a_size * 0.2F, bubbleMax.y },
                ImVec2{ a_min.x + a_size * 0.5F, bubbleMax.y }, ImVec2{ a_min.x + a_size * 0.15F, a_min.y + a_size },
                a_color);
            a_list.AddLine(ImVec2{ a_min.x, a_min.y + a_size }, ImVec2{ a_min.x + a_size, a_min.y }, a_color, a_thickness);
        }

        void DrawVanishedMark(ImDrawList& a_list, const ImVec2& a_min, float a_size, ImU32 a_color, float a_thickness)
        {
            const ImVec2 center{ a_min.x + a_size * 0.5F, a_min.y + a_size * 0.5F };
            a_list.PathLineTo(ImVec2{ a_min.x, center.y });
            a_list.PathBezierQuadraticCurveTo(ImVec2{ center.x, a_min.y - a_size * 0.1F }, ImVec2{ a_min.x + a_size, center.y });
            a_list.PathBezierQuadraticCurveTo(ImVec2{ center.x, a_min.y + a_size * 1.1F }, ImVec2{ a_min.x, center.y });
            a_list.PathStroke(a_color, ImDrawFlags_Closed, a_thickness);
            a_list.AddCircleFilled(center, a_size * 0.16F, a_color);
            a_list.AddLine(ImVec2{ a_min.x + a_size * 0.1F, a_min.y + a_size * 0.95F },
                ImVec2{ a_min.x + a_size * 0.9F, a_min.y + a_size * 0.05F }, a_color, a_thickness);
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

        const auto color = open ? Theme::GetAccent() : style.Colors[online ? ImGuiCol_Text : ImGuiCol_TextDisabled];
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
            previousNearby.swap(nearby);
            nearby.clear();
            ChatRoster::CopyNearbyPlayers(nearby);
            if (nearby != previousNearby)
            {
                Match();
            }
        }
        if (const auto current = PlayerFilters::GetRevision(); current != filtersRevision)
        {
            filtersRevision = current;
            std::vector<PlayerFilters::Entry> entries;
            PlayerFilters::Copy(entries);
            muted.clear();
            vanished.clear();
            for (const auto& entry : entries)
            {
                if (entry.muted)
                {
                    muted.push_back(entry.session);
                }
                if (entry.vanished)
                {
                    vanished.push_back(entry.session);
                }
            }
            std::ranges::sort(muted);
            std::ranges::sort(vanished);
        }
    }

    void ChatPlayers::Match()
    {
        matches.clear();
        for (std::uint32_t index = 0; index < players.size(); ++index)
        {
            const auto& entry = players[index];
            if (entry.player.playerID == localPlayer ||
                (!foldedSearch.empty() && entry.folded.find(foldedSearch) == std::string::npos))
            {
                continue;
            }
            matches.push_back(index);
        }
        const auto others = std::ranges::stable_partition(matches,
            [this](std::uint32_t a_index) { return std::ranges::binary_search(nearby, players[a_index].player.playerID); });
        nearbyMatches = static_cast<std::size_t>(others.begin() - matches.begin());
    }

    ChatPlayers::ListOutcome ChatPlayers::DrawList(float a_ruleGap, bool a_closeMenu)
    {
        Refresh(ImGui::GetTime());

        // As high as the chat's filters, so the rules under both line up.
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ChatStyle::GetChipPadding());
        if (Controls::SearchField("##ChatPlayerSearch", search))
        {
            foldedSearch = ChatRoster::FoldName(search.data());
            Match();
        }
        ImGui::PopStyleVar();
        ChatStyle::DrawRule(a_ruleGap);

        ListOutcome outcome;
        DrawRows(outcome.whisper, a_closeMenu);
        outcome.manage = ImGui::Button("Manage players", ImVec2{ -FLT_MIN, 0.0F });

        shownMin = ImGui::GetWindowPos();
        const auto size = ImGui::GetWindowSize();
        shownMax = ImVec2{ shownMin.x + size.x, shownMin.y + size.y };
        return outcome;
    }

    void ChatPlayers::DrawRows(std::optional<Multiplayer::RosterEntry>& a_picked, bool a_closeMenu)
    {
        if (ImGui::BeginChild("##ChatPlayerRows", ImVec2{ 0.0F, -ImGui::GetFrameHeightWithSpacing() }))
        {
            if (players.empty())
            {
                Controls::MutedTextWrapped("Not connected to the server.");
            }
            else if (matches.empty())
            {
                Controls::MutedTextWrapped(
                    foldedSearch.empty() ? "No other players online" : "No player with that name is online");
            }

            // The chat's field is active while the player types, which would
            // otherwise keep the rows from being hovered.
            constexpr auto kHover = ImGuiHoveredFlags_AllowWhenBlockedByActiveItem;
            const auto& style = ImGui::GetStyle();
            const auto& mutedColor = style.Colors[ImGuiCol_TextDisabled];
            const auto lineHeight = ImGui::GetTextLineHeight();
            const auto pitch = ChatHistory::GetLinePitch();
            const auto markSize = std::round(kFilterMarkFonts * ImGui::GetFontSize());
            const auto markSpace = markSize + style.ItemSpacing.x;
            const auto markLine = (std::max)(std::floor(ChatStyle::kChipOutlineFonts * ImGui::GetFontSize()), 1.0F);
            auto* drawList = ImGui::GetWindowDrawList();

            // Rows a chat line apart, so the clipper can skip the ones out of view.
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{ style.ItemSpacing.x, pitch - lineHeight });
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(matches.size()), pitch);
            while (clipper.Step())
            {
                for (auto row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    const auto index = static_cast<std::size_t>(row);
                    const auto& player = players[matches[index]].player;
                    ImGui::PushID(static_cast<int>(player.playerID));
                    const bool clicked = ImGui::InvisibleButton(
                        "##ChatPlayer", ImVec2{ (std::max)(ImGui::GetContentRegionAvail().x, 1.0F), lineHeight });
                    ImGui::PopID();
                    const bool hovered = ImGui::IsItemHovered(kHover);
                    const auto min = ImGui::GetItemRectMin();
                    const auto max = ImGui::GetItemRectMax();
                    const char* name = player.name.c_str();
                    const char* nameEnd = name + player.name.size();
                    const bool isMuted = std::ranges::binary_search(muted, player.session);
                    const bool isVanished = std::ranges::binary_search(vanished, player.session);
                    const auto marksLeft = max.x - markSpace * static_cast<float>(isMuted + isVanished);
                    const ImVec4 nameClip{ min.x, min.y, marksLeft, max.y };

                    const auto nameColor = ImGui::GetColorU32(
                        index < nearbyMatches ? ChatStyle::kLocalColor : kDistantPlayerColor);
                    drawList->AddText(nullptr, 0.0F, min, nameColor, name, nameEnd, 0.0F, &nameClip);
                    if (isMuted || isVanished)
                    {
                        const auto markColor = ImGui::GetColorU32(mutedColor);
                        ImVec2 markMin{ marksLeft + style.ItemSpacing.x, std::floor((min.y + max.y - markSize) * 0.5F) };
                        if (isMuted)
                        {
                            DrawMutedMark(*drawList, markMin, markSize, markColor, markLine);
                            markMin.x += markSpace;
                        }
                        if (isVanished)
                        {
                            DrawVanishedMark(*drawList, markMin, markSize, markColor, markLine);
                        }
                        if (hovered && ImGui::IsMouseHoveringRect(ImVec2{ marksLeft, min.y }, max))
                        {
                            ImGui::SetTooltip("%s", isMuted && isVanished ? "Muted and vanished"
                                    : isMuted                            ? "Muted"
                                                                         : "Vanished");
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
                        playerMenu.Open(player);
                        ImGui::OpenPopup("##ChatPlayerMenu");
                    }
                }
            }
            ImGui::PopStyleVar();

            menuOpen = ImGui::BeginPopup("##ChatPlayerMenu");
            if (menuOpen)
            {
                if (a_closeMenu)
                {
                    ImGui::CloseCurrentPopup();
                }
                if (playerMenu.Draw())
                {
                    a_picked = playerMenu.GetPlayer();
                }
                ImGui::EndPopup();
            }
        }
        ImGui::EndChild();
    }
}
