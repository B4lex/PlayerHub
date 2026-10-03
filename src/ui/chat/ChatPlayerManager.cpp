#include "ui/chat/ChatPlayerManager.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"
#include "ui/chat/ChatStyle.h"
#include "ui/widgets/Controls.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>

namespace UI
{
    namespace
    {
        // In font sizes, so the window follows the overlay scale.
        constexpr ImVec2 kSizeFonts{ 26.0F, 22.0F };
        // Of the base font size.
        constexpr float kTitleScale = 1.25F;
        constexpr float kTabSpacingFonts = 0.375F;

        struct TabLabel
        {
            const char* label;
            // The button that undoes the tab's choice.
            const char* action;
            const char* empty;
        };

        constexpr std::array kTabs{
            TabLabel{ "Friends", nullptr, nullptr },
            TabLabel{ "Muted", "Unmute", "You have not muted anyone on this server." },
            TabLabel{ "Vanished", "Unvanish", "You have not vanished anyone on this server." },
        };

        // Tab would move the keyboard off the field; the field keeps it.
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
        if (tab != Tab::kFriends)
        {
            PlayerFilters::Copy(entries);
            for (auto& entry : entries)
            {
                if (!(tab == Tab::kMuted ? entry.muted : entry.vanished))
                {
                    continue;
                }
                Row row{ .player = { .name = std::move(entry.name), .session = entry.session } };
                const auto found = std::ranges::lower_bound(
                    online, entry.session, {}, &std::pair<Multiplayer::SessionID, std::uint32_t>::first);
                if (found != online.end() && found->first == entry.session)
                {
                    // The name the player has now, which a rename changed.
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
            std::ranges::sort(rows, {}, &Row::folded);
        }
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

    void ChatPlayerManager::Draw()
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
            return;
        }
        ImGui::SetCursorPosY((std::max)(titleBottom, ImGui::GetCursorPosY()));

        DrawTabs();
        ChatStyle::DrawRule(style.ItemSpacing.y);
        if (tab == Tab::kFriends)
        {
            return;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ChatStyle::GetChipPadding());
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##ManagePlayersSearch", "Search", search.data(), search.size(),
                ImGuiInputTextFlags_CallbackCompletion, &KeepTab))
        {
            foldedSearch = ChatRoster::FoldName(search.data());
            Match();
        }
        ImGui::PopStyleVar();
        DrawRows();
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

            // The tab shown is a chip in the accent, as the player list's
            // button is while the list shows; the others are outlines.
            const auto& color
                = selected ? Theme::kAccent : style.Colors[hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled];
            const auto fillAlpha = selected ? (hovered ? ChatStyle::kHoveredChipFillAlpha : ChatStyle::kChipFillAlpha)
                                            : (hovered ? ChatStyle::kHoveredOffChipFillAlpha : 0.0F);
            ChatStyle::DrawChipFrame(color, fillAlpha);
            const auto min = ImGui::GetItemRectMin();
            drawList->AddText(ImVec2{ min.x + padding.x, min.y + padding.y }, ImGui::GetColorU32(color), label);
        }
    }

    void ChatPlayerManager::DrawRows()
    {
        if (!ImGui::BeginChild("##ManagePlayersRows"))
        {
            ImGui::EndChild();
            return;
        }
        const auto& current = kTabs[static_cast<std::size_t>(tab)];
        if (rows.empty())
        {
            DrawMuted(current.empty);
        }
        else if (matches.empty())
        {
            DrawMuted("No player with that name");
        }

        const auto& style = ImGui::GetStyle();
        const auto buttonWidth = (std::max)(ImGui::CalcTextSize(kTabs[1].action).x, ImGui::CalcTextSize(kTabs[2].action).x)
            + style.FramePadding.x * 2.0F;
        const auto onlineWidth = ImGui::CalcTextSize("Online").x;
        const auto right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        const auto buttonLeft = right - buttonWidth;
        const auto onlineLeft = buttonLeft - style.ItemSpacing.x - onlineWidth;

        std::optional<Multiplayer::RosterEntry> undone;
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(matches.size()), ImGui::GetFrameHeightWithSpacing());
        while (clipper.Step())
        {
            for (auto index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
            {
                const auto& row = rows[matches[static_cast<std::size_t>(index)]];
                const auto& name = row.player.name;
                ImGui::PushID(index);
                ImGui::AlignTextToFramePadding();
                const auto nameMin = ImGui::GetCursorScreenPos();
                const auto windowLeft = ImGui::GetWindowPos().x;
                ImGui::PushClipRect(nameMin,
                    ImVec2{ windowLeft + onlineLeft - style.ItemSpacing.x, nameMin.y + ImGui::GetFrameHeight() }, true);
                ImGui::TextUnformatted(name.c_str(), name.c_str() + name.size());
                ImGui::PopClipRect();
                if (row.online)
                {
                    ImGui::SameLine(onlineLeft);
                    ImGui::TextDisabled("Online");
                }
                ImGui::SameLine(buttonLeft);
                if (ImGui::Button(current.action, ImVec2{ buttonWidth, 0.0F }))
                {
                    undone = row.player;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        // The rows change with it, so after the clipper is done with them.
        if (undone)
        {
            if (tab == Tab::kMuted)
            {
                PlayerFilters::SetMuted(*undone, false);
            }
            else
            {
                PlayerFilters::SetVanished(*undone, false);
            }
        }
    }
}
