#include "ui/chat/ChatPlayers.h"
#include "sync/filters/PlayerFilters.h"
#include "sync/voicechat/VoiceChatSync.h"
#include "text/Text.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"
#include "ui/chat/ChatHistory.h"
#include "ui/chat/ChatStyle.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/RoleBadge.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>

namespace UI
{
    namespace
    {
        constexpr float kListWidthFonts = 13.0F;
        constexpr float kFilterMarkFonts = 0.75F;
        // U+E003, U+E004, U+E00B, and U+E00C of the logo font.
        constexpr const char* kMutedGlyph = "\xEE\x80\x83";
        constexpr const char* kVanishedGlyph = "\xEE\x80\x84";
        constexpr const char* kPersonGlyph = "\xEE\x80\x8B";
        constexpr const char* kSpeakerGlyph = "\xEE\x80\x8C";
        constexpr double kNearbySeconds = 0.5;
        constexpr ImVec4 kDistantPlayerColor{ 0.76F, 0.75F, 0.73F, 1.0F };
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
        const char* text = online ? label.data() : Text::GetCString(Text::ID::kPlayersOffline);

        const auto& style = ImGui::GetStyle();
        const auto fontSize = ImGui::GetFontSize();
        const auto padding = ChatStyle::GetChipPadding();
        const auto textSize = ImGui::CalcTextSize(text);
        const auto digit = ChatStyle::FindDigitRows();
        auto* const iconFont = online ? Fonts::GetLogo() : nullptr;
        const auto icon =
            iconFont ? std::round(iconFont->CalcTextSizeA(digit.height, FLT_MAX, 0.0F, kPersonGlyph).x) : 0.0F;
        const auto gap = iconFont ? std::floor(ChatStyle::kIconGapFonts * fontSize) : 0.0F;
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
        if (iconFont)
        {
            drawList->AddText(iconFont, digit.height, ImVec2{ content.x, content.y + digit.top }, ImGui::GetColorU32(color),
                kPersonGlyph);
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
        outcome.manage = ImGui::Button(Text::GetCString(Text::ID::kControlsManagePlayers), ImVec2{ -FLT_MIN, 0.0F });

        shownMin = ImGui::GetWindowPos();
        const auto size = ImGui::GetWindowSize();
        shownMax = ImVec2{ shownMin.x + size.x, shownMin.y + size.y };
        return outcome;
    }

    void ChatPlayers::DrawRows(std::optional<Multiplayer::PlayerIdentity>& a_picked, bool a_closeMenu)
    {
        if (ImGui::BeginChild("##ChatPlayerRows", ImVec2{ 0.0F, -ImGui::GetFrameHeightWithSpacing() }))
        {
            if (players.empty())
            {
                Controls::MutedTextWrapped(Text::GetCString(Text::ID::kChatNotConnected));
            }
            else if (matches.empty())
            {
                Controls::MutedTextWrapped(
                    Text::GetCString(foldedSearch.empty() ? Text::ID::kChatNoOtherPlayers : Text::ID::kChatNoPlayerNamed));
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
            const auto roleSize = RoleBadge::GetMarkSize(ImGui::GetFontSize());
            const auto roleSpace = roleSize + RoleBadge::GetMarkGap(ImGui::GetFontSize());
            auto* markFont = Fonts::GetLogo();
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
                    const auto& entry = players[matches[index]];
                    const auto& player = entry.player;
                    ImGui::PushID(static_cast<int>(player.playerID));
                    const bool clicked = ImGui::InvisibleButton(
                        "##ChatPlayer", ImVec2{ (std::max)(ImGui::GetContentRegionAvail().x, 1.0F), lineHeight });
                    ImGui::PopID();
                    const bool hovered = ImGui::IsItemHovered(kHover);
                    const auto min = ImGui::GetItemRectMin();
                    const auto max = ImGui::GetItemRectMax();
                    const char* name = player.name.c_str();
                    const char* nameEnd = name + player.name.size();
                    const bool isMuted = markFont && std::ranges::binary_search(muted, player.session);
                    const bool isVanished = markFont && std::ranges::binary_search(vanished, player.session);
                    const bool isSpeaking = markFont && VoiceChatSync::IsSpeaking(player.playerID);
                    const auto marksLeft = max.x - markSpace * static_cast<float>(isMuted + isVanished + isSpeaking);
                    const ImVec4 nameClip{ min.x, min.y, marksLeft, max.y };
                    const ImVec2 nameMin{ min.x + (entry.role ? roleSpace : 0.0F), min.y };

                    const auto nameColor = ImGui::GetColorU32(
                        index < nearbyMatches ? ChatStyle::kNearbyColor : kDistantPlayerColor);
                    if (entry.role)
                    {
                        RoleBadge::DrawMark(*drawList, ImVec2{ min.x, std::floor((min.y + max.y - roleSize) * 0.5F) },
                            roleSize, entry.role, style.Alpha);
                    }
                    drawList->AddText(nullptr, 0.0F, nameMin, nameColor, name, nameEnd, 0.0F, &nameClip);
                    if (isSpeaking)
                    {
                        // Ahead of the marks, where the player looks for who is talking.
                        const ImVec2 speakingMin{ marksLeft + style.ItemSpacing.x,
                            std::floor((min.y + max.y - markSize) * 0.5F) };
                        drawList->AddText(markFont, markSize, speakingMin, ImGui::GetColorU32(Theme::GetAccent()),
                            kSpeakerGlyph);
                    }
                    if (isMuted || isVanished)
                    {
                        const auto markColor = ImGui::GetColorU32(mutedColor);
                        ImVec2 markMin{ marksLeft + style.ItemSpacing.x + (isSpeaking ? markSpace : 0.0F),
                            std::floor((min.y + max.y - markSize) * 0.5F) };
                        if (isMuted)
                        {
                            drawList->AddText(markFont, markSize, markMin, markColor, kMutedGlyph);
                            markMin.x += markSpace;
                        }
                        if (isVanished)
                        {
                            drawList->AddText(markFont, markSize, markMin, markColor, kVanishedGlyph);
                        }
                        if (hovered && ImGui::IsMouseHoveringRect(ImVec2{ marksLeft + (isSpeaking ? markSpace : 0.0F), min.y }, max))
                        {
                            const auto marks = isMuted && isVanished ? Text::ID::kChatMutedAndVanished
                                : isMuted                            ? Text::ID::kPlayersMuted
                                                                     : Text::ID::kPlayersVanished;
                            ImGui::SetTooltip("%s", Text::GetCString(marks));
                        }
                    }
                    if (!hovered)
                    {
                        continue;
                    }
                    const auto underline = (std::min)(nameMin.x + ImGui::CalcTextSize(name, nameEnd).x, nameClip.z);
                    drawList->AddRectFilled(ImVec2{ nameMin.x, max.y - 1.0F }, ImVec2{ underline, max.y }, nameColor);
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
