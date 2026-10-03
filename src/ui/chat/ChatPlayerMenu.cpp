#include "ui/chat/ChatPlayerMenu.h"
#include "sync/chat/ChatRoster.h"
#include "sync/filters/PlayerFilters.h"

#include <imgui.h>

namespace UI::ChatPlayerMenu
{
    void DrawFilterItems(const Multiplayer::RosterEntry& a_player)
    {
        if (a_player.session == 0 || a_player.session == ChatRoster::GetLocalSession())
        {
            return;
        }
        ImGui::Separator();
        const bool muted = PlayerFilters::IsMuted(a_player.session);
        if (ImGui::MenuItem(muted ? "Unmute" : "Mute"))
        {
            PlayerFilters::SetMuted(a_player, !muted);
        }
        ImGui::SetItemTooltip(
            "%s", muted ? "Show their messages again." : "Hide their messages in every channel.");
        const bool vanished = PlayerFilters::IsVanished(a_player.session);
        if (ImGui::MenuItem(vanished ? "Unvanish" : "Vanish"))
        {
            PlayerFilters::SetVanished(a_player, !vanished);
        }
        ImGui::SetItemTooltip("%s", vanished ? "Show their character in your world again."
                                             : "Remove their character from your world.");
    }
}
