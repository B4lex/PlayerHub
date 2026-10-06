#include "ui/chat/ChatPlayerMenu.h"
#include "sync/chat/ChatRoster.h"
#include "sync/chat/PlayerDetails.h"
#include "sync/filters/PlayerFilters.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <format>

namespace UI
{
    namespace
    {
        constexpr float kDetailsWaitSeconds = 2.0F;
        constexpr float kMaximumFrameSeconds = 0.1F;

        std::string FormatDetails(const PlayerDetails::Details& a_details)
        {
            if (a_details.level == 0)
            {
                return a_details.race;
            }
            if (a_details.race.empty())
            {
                return std::format("Level {}", a_details.level);
            }
            return std::format("{} \xC2\xB7 Level {}", a_details.race, a_details.level);
        }
    }

    void ChatPlayerMenu::Open(const Multiplayer::RosterEntry& a_player)
    {
        player = a_player;
        details.clear();
        waited = 0.0F;
        pending = player.playerID != 0;
        if (pending)
        {
            PlayerDetails::Request(player.playerID);
        }
    }

    bool ChatPlayerMenu::Draw()
    {
        DrawHeading();
        const bool whisper = ImGui::MenuItem("Whisper");
        DrawFilterItems();
        return whisper;
    }

    void ChatPlayerMenu::DrawHeading()
    {
        if (pending)
        {
            if (const auto found = PlayerDetails::Find(player.playerID))
            {
                details = FormatDetails(*found);
                pending = false;
            }
            else
            {
                waited += (std::min)(ImGui::GetIO().DeltaTime, kMaximumFrameSeconds);
                pending = waited < kDetailsWaitSeconds;
            }
        }
        ImGui::PushFont(Fonts::GetHeading(), 0.0F);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::GetAccent());
        ImGui::TextUnformatted(player.name.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        if (pending)
        {
            // Holds the line's height until the details arrive, so the menu does not grow under the cursor.
            ImGui::Dummy(ImVec2{ 0.0F, ImGui::GetTextLineHeight() });
        }
        else if (!details.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextUnformatted(details.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Separator();
    }

    void ChatPlayerMenu::DrawFilterItems() const
    {
        if (player.session == 0 || player.session == ChatRoster::GetLocalSession())
        {
            return;
        }
        ImGui::Separator();
        const bool muted = PlayerFilters::IsMuted(player.session);
        if (ImGui::MenuItem(muted ? "Unmute" : "Mute"))
        {
            PlayerFilters::SetMuted(player, !muted);
        }
        ImGui::SetItemTooltip("%s", muted ? "Show their messages again." : "Hide their messages in every channel.");
        const bool vanished = PlayerFilters::IsVanished(player.session);
        if (ImGui::MenuItem(vanished ? "Unvanish" : "Vanish"))
        {
            PlayerFilters::SetVanished(player, !vanished);
        }
        ImGui::SetItemTooltip(
            "%s", vanished ? "Show their character in your world again." : "Remove their character from your world.");
    }
}
