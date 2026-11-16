#include "ui/chat/ChatPlayerMenu.h"
#include "sync/chat/ChatRoster.h"
#include "sync/chat/PlayerDetails.h"
#include "sync/filters/PlayerFilters.h"
#include "sync/trade/TradeSync.h"
#include "text/Text.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <utility>

namespace UI
{
    namespace
    {
        using Text::Id;

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
                return Text::Format(Id::kPlayerMenuLevel, a_details.level);
            }
            return Text::Format(Id::kPlayerMenuRaceLevel, a_details.race, a_details.level);
        }
    }

    void ChatPlayerMenu::Open(const Multiplayer::PlayerIdentity& a_player)
    {
        player = a_player;
        const auto known = PlayerDetails::FindKnown(player.session);
        details = known ? FormatDetails(*known) : std::string{};
        inTradeRange = false;
        waited = 0.0F;
        pending = player.playerID != 0;
        if (pending)
        {
            PlayerDetails::Request(player.playerID, player.session);
        }
    }

    bool ChatPlayerMenu::Draw()
    {
        DrawHeading();
        // An offline player has no slot to whisper.
        const bool whisper = ImGui::MenuItem(Text::GetCString(Id::kPlayerMenuWhisper), nullptr, false, player.playerID != 0);
        DrawTradeItems();
        DrawFilterItems();
        return whisper;
    }

    void ChatPlayerMenu::DrawHeading()
    {
        if (pending)
        {
            if (const auto found = PlayerDetails::Find(player.playerID))
            {
                // An empty answer, such as one without a connection, keeps the known details.
                if (auto text = FormatDetails(*found); !text.empty())
                {
                    details = std::move(text);
                }
                inTradeRange = found->inTradeRange;
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
        if (pending && details.empty())
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

    void ChatPlayerMenu::DrawTradeItems() const
    {
        if (player.playerID == 0 || player.session == 0 || player.session == ChatRoster::GetLocalSession())
        {
            return;
        }
        if (TradeSync::GetInvitingPlayer() == player.playerID)
        {
            if (ImGui::MenuItem(Text::GetCString(Id::kPlayerMenuAcceptTrade)))
            {
                TradeSync::AnswerInvitation(true);
            }
            if (ImGui::MenuItem(Text::GetCString(Id::kPlayerMenuDeclineTrade)))
            {
                TradeSync::AnswerInvitation(false);
            }
            return;
        }
        const bool idle = TradeSync::GetPhase() == TradeSession::Phase::kIdle;
        if (ImGui::MenuItem(Text::GetCString(Id::kPlayerMenuTrade), nullptr, false, idle && inTradeRange))
        {
            TradeSync::RequestTrade(player);
        }
        const auto tooltip = !idle ? Id::kPlayerMenuTradeBusy
            : inTradeRange         ? Id::kPlayerMenuTradeHint
            : pending              ? Id::kPlayerMenuTradeChecking
                                   : Id::kPlayerMenuTradeTooFar;
        ImGui::SetItemTooltip("%s", Text::GetCString(tooltip));
    }

    void ChatPlayerMenu::DrawFilterItems() const
    {
        if (player.session == 0 || player.session == ChatRoster::GetLocalSession())
        {
            return;
        }
        const bool friended = PlayerFilters::IsFriend(player.session);
        if (ImGui::MenuItem(Text::GetCString(friended ? Id::kPlayerMenuRemoveFriend : Id::kPlayerMenuAddFriend)))
        {
            PlayerFilters::SetFriend(player, !friended);
        }
        ImGui::SetItemTooltip(
            "%s", Text::GetCString(friended ? Id::kPlayerMenuRemoveFriendHint : Id::kPlayerMenuAddFriendHint));
        ImGui::Separator();
        const bool muted = PlayerFilters::IsMuted(player.session);
        if (ImGui::MenuItem(Text::GetCString(muted ? Id::kPlayersUnmute : Id::kPlayerMenuMute)))
        {
            PlayerFilters::SetMuted(player, !muted);
        }
        ImGui::SetItemTooltip("%s", Text::GetCString(muted ? Id::kPlayerMenuUnmuteHint : Id::kPlayerMenuMuteHint));
        const bool vanished = PlayerFilters::IsVanished(player.session);
        if (ImGui::MenuItem(Text::GetCString(vanished ? Id::kPlayersUnvanish : Id::kPlayerMenuVanish)))
        {
            PlayerFilters::SetVanished(player, !vanished);
        }
        ImGui::SetItemTooltip(
            "%s", Text::GetCString(vanished ? Id::kPlayerMenuUnvanishHint : Id::kPlayerMenuVanishHint));
    }
}
