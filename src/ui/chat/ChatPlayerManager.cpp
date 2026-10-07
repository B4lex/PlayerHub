#include "ui/chat/ChatPlayerManager.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"
#include "ui/chat/ChatStyle.h"
#include "ui/widgets/Controls.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace UI
{
    namespace
    {
        constexpr ImVec2 kSizeFonts{ 26.0F, 22.0F };
        constexpr float kTitleScale = 1.25F;
        constexpr float kTabSpacingFonts = 0.375F;
        constexpr float kStatusDotFonts = 0.4375F;
        constexpr ImVec4 kOnlineColor{ 0.35F, 0.85F, 0.40F, 1.0F };

        struct TabLabel
        {
            const char* label;
            bool PlayerFilters::Entry::* listed;
            const char* action;
            const char* empty;
        };

        constexpr std::array kTabs{
            TabLabel{ "Friends", &PlayerFilters::Entry::friended, "Remove",
                "You have not added any friends on this server." },
            TabLabel{ "Muted", &PlayerFilters::Entry::muted, "Unmute", "You have not muted anyone on this server." },
            TabLabel{ "Vanished", &PlayerFilters::Entry::vanished, "Unvanish",
                "You have not vanished anyone on this server." },
        };

    }

    ImVec2 ChatPlayerManager::GetSize()
    {
        const auto fontSize = ImGui::GetFontSize();
        return ImVec2{ std::floor(kSizeFonts.x * fontSize), std::floor(kSizeFonts.y * fontSize) };
    }

    bool ChatPlayerManager::IsHovered() const
    {
        return open && ImGui::IsMouseHoveringRect(shownMin, shownMax, false);
    }

    void ChatPlayerManager::Open()
    {
        open = true;
        focusRequested = true;
    }

    void ChatPlayerManager::Close()
    {
        open = false;
        tab = Tab::kFriends;
        search[0] = '\0';
        foldedSearch.clear();
        stale = true;
        menuOpen = false;
    }

    void ChatPlayerManager::Refresh()
    {
        if (ChatRoster::CopyPlayersIfChanged(rosterRevision, roster, localPlayer))
        {
            online.clear();
            for (std::uint32_t index = 0; index < roster.size(); ++index)
            {
                online.emplace_back(roster[index].player.session, index);
            }
            std::ranges::sort(online);
            stale = true;
        }
        if (const auto revision = PlayerFilters::GetRevision(); revision != filtersRevision)
        {
            filtersRevision = revision;
            stale = true;
        }
        if (!stale)
        {
            return;
        }
        stale = false;

        rows.clear();
        PlayerFilters::Copy(entries);
        const auto shown = kTabs[static_cast<std::size_t>(tab)].listed;
        for (auto& entry : entries)
        {
            if (!(entry.*shown))
            {
                continue;
            }
            Row row{ .player = { .name = std::move(entry.name), .session = entry.session } };
            const auto found = std::ranges::lower_bound(
                online, entry.session, {}, &std::pair<Multiplayer::SessionID, std::uint32_t>::first);
            if (found != online.end() && found->first == entry.session)
            {
                const auto& listed = roster[found->second];
                row.player = listed.player;
                row.folded = listed.folded;
                row.online = true;
            }
            else
            {
                row.folded = ChatRoster::FoldName(row.player.name);
            }
            rows.push_back(std::move(row));
        }
        std::ranges::sort(rows, [](const Row& a_left, const Row& a_right) {
            return a_left.online != a_right.online ? a_left.online : a_left.folded < a_right.folded;
        });
        Match();
    }

    void ChatPlayerManager::Match()
    {
        matches.clear();
        for (std::uint32_t index = 0; index < rows.size(); ++index)
        {
            if (foldedSearch.empty() || rows[index].folded.find(foldedSearch) != std::string::npos)
            {
                matches.push_back(index);
            }
        }
    }

    std::optional<Multiplayer::RosterEntry> ChatPlayerManager::Draw(bool a_closeMenu)
    {
        Refresh();
        const auto& style = ImGui::GetStyle();
        shownMin = ImGui::GetWindowPos();
        const auto windowSize = ImGui::GetWindowSize();
        shownMax = ImVec2{ shownMin.x + windowSize.x, shownMin.y + windowSize.y };

        const auto top = ImGui::GetCursorPosY();
        const auto right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        ImGui::PushFont(Fonts::GetHeading(), style.FontSizeBase * kTitleScale);
        ImGui::TextUnformatted("Players");
        ImGui::PopFont();
        const auto titleBottom = ImGui::GetCursorPosY();
        ImGui::SetCursorPos(ImVec2{ right - ImGui::GetFrameHeight(), top });
        if (Controls::IconButton("##ManagePlayersClose", Controls::Icon::kClose))
        {
            Close();
            return std::nullopt;
        }
        ImGui::SetCursorPosY((std::max)(titleBottom, ImGui::GetCursorPosY()));

        DrawTabs();
        ChatStyle::DrawRule(style.ItemSpacing.y);

        if (!rows.empty())
        {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ChatStyle::GetChipPadding());
            if (Controls::SearchField("##ManagePlayersSearch", search))
            {
                foldedSearch = ChatRoster::FoldName(search.data());
                Match();
            }
            ImGui::PopStyleVar();
        }
        std::optional<Multiplayer::RosterEntry> whisper;
        DrawRows(whisper, a_closeMenu);
        return whisper;
    }

    void ChatPlayerManager::DrawTabs()
    {
        const auto& style = ImGui::GetStyle();
        const auto padding = ChatStyle::GetChipPadding();
        auto* drawList = ImGui::GetWindowDrawList();
        for (std::size_t index = 0; index < kTabs.size(); ++index)
        {
            const auto* label = kTabs[index].label;
            if (index > 0)
            {
                ImGui::SameLine(0.0F, std::floor(kTabSpacingFonts * ImGui::GetFontSize()));
            }
            const auto textSize = ImGui::CalcTextSize(label);
            ImGui::PushID(static_cast<int>(index));
            const bool pressed = ImGui::InvisibleButton(
                "##ManagePlayersTab", ImVec2{ textSize.x + padding.x * 2.0F, textSize.y + padding.y * 2.0F });
            ImGui::PopID();
            const bool hovered = ImGui::IsItemHovered();
            const bool selected = static_cast<std::size_t>(tab) == index;
            if (pressed && !selected)
            {
                tab = static_cast<Tab>(index);
                stale = true;
            }
            if (hovered)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }

            const auto color
                = selected ? Theme::GetAccent() : style.Colors[hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled];
            const auto fillAlpha = selected ? (hovered ? ChatStyle::kHoveredChipFillAlpha : ChatStyle::kChipFillAlpha)
                                            : (hovered ? ChatStyle::kHoveredOffChipFillAlpha : 0.0F);
            ChatStyle::DrawChipFrame(color, fillAlpha);
            const auto min = ImGui::GetItemRectMin();
            drawList->AddText(ImVec2{ min.x + padding.x, min.y + padding.y }, ImGui::GetColorU32(color), label);
        }
    }

    void ChatPlayerManager::DrawRows(std::optional<Multiplayer::RosterEntry>& a_whisper, bool a_closeMenu)
    {
        if (!ImGui::BeginChild("##ManagePlayersRows"))
        {
            ImGui::EndChild();
            return;
        }
        const auto& current = kTabs[static_cast<std::size_t>(tab)];
        if (rows.empty())
        {
            Controls::MutedTextWrapped(current.empty);
        }
        else if (matches.empty())
        {
            Controls::MutedTextWrapped("No player with that name");
        }

        // The chat's field keeps the keyboard, which would otherwise keep the rows from being hovered.
        constexpr auto kHover = ImGuiHoveredFlags_AllowWhenBlockedByActiveItem;
        const auto& style = ImGui::GetStyle();
        auto buttonWidth = 0.0F;
        for (const auto& label : kTabs)
        {
            buttonWidth = (std::max)(buttonWidth, ImGui::CalcTextSize(label.action).x);
        }
        buttonWidth += style.FramePadding.x * 2.0F;
        const auto right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        const auto buttonLeft = right - buttonWidth;
        const auto frameHeight = ImGui::GetFrameHeight();
        const auto dotSize = std::round(kStatusDotFonts * ImGui::GetFontSize());
        const auto offlineColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        const auto textColor = ImGui::GetColorU32(ImGuiCol_Text);
        auto* drawList = ImGui::GetWindowDrawList();

        std::optional<Multiplayer::RosterEntry> undone;
        std::optional<Multiplayer::RosterEntry> menuPlayer;
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(matches.size()), ImGui::GetFrameHeightWithSpacing());
        while (clipper.Step())
        {
            for (auto index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
            {
                const auto& row = rows[matches[static_cast<std::size_t>(index)]];
                const char* name = row.player.name.c_str();
                const char* nameEnd = name + row.player.name.size();
                ImGui::PushID(index);

                ImGui::Dummy(ImVec2{ dotSize, frameHeight });
                const auto dotMin = ImGui::GetItemRectMin();
                drawList->AddCircleFilled(ImVec2{ dotMin.x + dotSize * 0.5F, dotMin.y + frameHeight * 0.5F },
                    dotSize * 0.5F, row.online ? ImGui::GetColorU32(kOnlineColor) : offlineColor);
                if (ImGui::IsItemHovered(kHover))
                {
                    ImGui::SetTooltip("%s", row.online ? "Online" : "Offline");
                }

                ImGui::SameLine();
                const auto nameLeft = ImGui::GetCursorPosX();
                const auto columnWidth = (std::max)(buttonLeft - style.ItemSpacing.x - nameLeft, 1.0F);
                const auto nameWidth = (std::min)(ImGui::CalcTextSize(name, nameEnd).x, columnWidth);
                const bool clicked = ImGui::InvisibleButton("##ManagePlayersName", ImVec2{ nameWidth, frameHeight });
                const auto min = ImGui::GetItemRectMin();
                const auto max = ImGui::GetItemRectMax();
                const ImVec4 clip{ min.x, min.y, max.x, max.y };
                const auto textTop = std::floor(min.y + (frameHeight - ImGui::GetTextLineHeight()) * 0.5F);
                drawList->AddText(nullptr, 0.0F, ImVec2{ min.x, textTop }, textColor, name, nameEnd, 0.0F, &clip);
                if (ImGui::IsItemHovered(kHover))
                {
                    const auto underline = textTop + ImGui::GetTextLineHeight();
                    drawList->AddRectFilled(ImVec2{ min.x, underline - 1.0F }, ImVec2{ max.x, underline }, textColor);
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (clicked && row.online)
                    {
                        a_whisper = row.player;
                    }
                    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right))
                    {
                        menuPlayer = row.player;
                    }
                }

                ImGui::SameLine(buttonLeft);
                if (ImGui::Button(current.action, ImVec2{ buttonWidth, 0.0F }))
                {
                    undone = row.player;
                }
                ImGui::PopID();
            }
        }

        if (menuPlayer)
        {
            playerMenu.Open(*menuPlayer);
            ImGui::OpenPopup("##ManagePlayersMenu");
        }
        menuOpen = ImGui::BeginPopup("##ManagePlayersMenu");
        if (menuOpen)
        {
            if (a_closeMenu)
            {
                ImGui::CloseCurrentPopup();
            }
            if (playerMenu.Draw())
            {
                a_whisper = playerMenu.GetPlayer();
            }
            ImGui::EndPopup();
        }
        ImGui::EndChild();

        // The rows change with it, so after the clipper is done with them.
        if (undone)
        {
            switch (tab)
            {
                case Tab::kFriends:
                    PlayerFilters::SetFriend(*undone, false);
                    break;
                case Tab::kMuted:
                    PlayerFilters::SetMuted(*undone, false);
                    break;
                case Tab::kVanished:
                    PlayerFilters::SetVanished(*undone, false);
                    break;
            }
        }
    }
}
