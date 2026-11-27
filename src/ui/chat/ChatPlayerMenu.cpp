#include "ui/chat/ChatPlayerMenu.h"
#include "sync/chat/ChatRoster.h"
#include "sync/chat/PlayerDetails.h"
#include "sync/filters/PlayerFilters.h"
#include "sync/trade/TradeSync.h"
#include "text/Text.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"
#include "ui/chat/ChatStyle.h"
#include "ui/widgets/RoleBadge.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <utility>

namespace UI
{
    namespace
    {
        using Text::ID;

        constexpr float kDetailsWaitSeconds = 2.0F;
        constexpr float kMaximumFrameSeconds = 0.1F;
        constexpr float kSecondaryTextScale = 0.85F;
        // U+E00D of the logo font.
        constexpr const char* kPlaceGlyph = "\xEE\x80\x8D";

        std::string FormatLevel(const PlayerDetails::Details& a_details)
        {
            return a_details.level != 0 ? Text::Format(ID::kPlayerMenuLevel, a_details.level) : std::string{};
        }

        std::string FormatPlace(const PlayerDetails::Details& a_details)
        {
            if (!a_details.answered)
            {
                return {};
            }
            if (!a_details.place)
            {
                return std::string{ Text::Get(ID::kPlayerMenuOutOfWorld) };
            }
            return a_details.place->empty() ? std::string{ Text::Get(ID::kPlayerMenuUnknownPlace) } : *a_details.place;
        }

        void DrawPlace(const std::string& a_place)
        {
            if (auto* const iconFont = Fonts::GetLogo())
            {
                const auto digit = ChatStyle::FindDigitRows();
                const auto width = std::round(iconFont->CalcTextSizeA(digit.height, FLT_MAX, 0.0F, kPlaceGlyph).x);
                const auto min = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2{ width, ImGui::GetTextLineHeight() });
                ImGui::GetWindowDrawList()->AddText(iconFont, digit.height, ImVec2{ min.x, min.y + digit.top },
                    ImGui::GetColorU32(Theme::GetAccent()), kPlaceGlyph);
                ImGui::SameLine(0.0F, std::floor(ChatStyle::kIconGapFonts * ImGui::GetFontSize()));
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextUnformatted(a_place.c_str());
            ImGui::PopStyleColor();
        }
    }

    void ChatPlayerMenu::Open(const Multiplayer::PlayerIdentity& a_player)
    {
        player = a_player;
        // A chat line carries the role its player had then; a connected player shows the one held now.
        const auto listed = ChatRoster::Find(player.playerID);
        role = ChatRoster::FindRole(listed && listed->session == player.session ? listed->role : player.role);
        const auto known = PlayerDetails::FindKnown(player.session);
        level = known ? FormatLevel(*known) : std::string{};
        place = known ? FormatPlace(*known) : std::string{};
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
        const bool whisper = ImGui::MenuItem(Text::GetCString(ID::kPlayerMenuWhisper), nullptr, false, player.playerID != 0);
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
                // The empty answer given without a connection keeps the known details.
                if (found->answered)
                {
                    level = FormatLevel(*found);
                    place = FormatPlace(*found);
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
        const auto nameTop = ImGui::GetItemRectMin().y;
        const auto nameAscent = ImGui::GetFontBaked()->Ascent;
        ImGui::PopStyleColor();
        ImGui::PopFont();
        if (!level.empty())
        {
            ImGui::SameLine();
            ImGui::PushFont(nullptr, Fonts::GetBaseSize() * kSecondaryTextScale);
            const auto min = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImGui::CalcTextSize(level.c_str()));
            // On the name's baseline, since the name's face and size differ.
            ImGui::GetWindowDrawList()->AddText(
                ImVec2{ min.x, nameTop + std::round(nameAscent - ImGui::GetFontBaked()->Ascent) },
                ImGui::GetColorU32(ImGuiCol_TextDisabled), level.c_str());
            ImGui::PopFont();
        }
        ImGui::PushFont(nullptr, Fonts::GetBaseSize() * kSecondaryTextScale);
        if (role)
        {
            RoleBadge::Name(*role);
        }
        if (!place.empty())
        {
            DrawPlace(place);
        }
        else if (pending && level.empty())
        {
            // Holds the line's height until the details arrive, so the menu does not grow under the cursor.
            ImGui::Dummy(ImVec2{ 0.0F, ImGui::GetTextLineHeight() });
        }
        ImGui::PopFont();
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
            if (ImGui::MenuItem(Text::GetCString(ID::kPlayerMenuAcceptTrade)))
            {
                TradeSync::AnswerInvitation(true);
            }
            if (ImGui::MenuItem(Text::GetCString(ID::kPlayerMenuDeclineTrade)))
            {
                TradeSync::AnswerInvitation(false);
            }
            return;
        }
        const bool idle = TradeSync::GetPhase() == TradeSession::Phase::kIdle;
        if (ImGui::MenuItem(Text::GetCString(ID::kPlayerMenuTrade), nullptr, false, idle && inTradeRange))
        {
            TradeSync::RequestTrade(player);
        }
        const auto tooltip = !idle ? ID::kPlayerMenuTradeBusy
            : inTradeRange         ? ID::kPlayerMenuTradeHint
            : pending              ? ID::kPlayerMenuTradeChecking
                                   : ID::kPlayerMenuTradeTooFar;
        ImGui::SetItemTooltip("%s", Text::GetCString(tooltip));
    }

    void ChatPlayerMenu::DrawFilterItems() const
    {
        if (player.session == 0 || player.session == ChatRoster::GetLocalSession())
        {
            return;
        }
        const bool friended = PlayerFilters::IsFriend(player.session);
        if (ImGui::MenuItem(Text::GetCString(friended ? ID::kPlayerMenuRemoveFriend : ID::kPlayerMenuAddFriend)))
        {
            PlayerFilters::SetFriend(player, !friended);
        }
        ImGui::SetItemTooltip(
            "%s", Text::GetCString(friended ? ID::kPlayerMenuRemoveFriendHint : ID::kPlayerMenuAddFriendHint));
        ImGui::Separator();
        const bool muted = PlayerFilters::IsMuted(player.session);
        if (ImGui::MenuItem(Text::GetCString(muted ? ID::kPlayersUnmute : ID::kPlayerMenuMute)))
        {
            PlayerFilters::SetMuted(player, !muted);
        }
        ImGui::SetItemTooltip("%s", Text::GetCString(muted ? ID::kPlayerMenuUnmuteHint : ID::kPlayerMenuMuteHint));
        const bool vanished = PlayerFilters::IsVanished(player.session);
        if (ImGui::MenuItem(Text::GetCString(vanished ? ID::kPlayersUnvanish : ID::kPlayerMenuVanish)))
        {
            PlayerFilters::SetVanished(player, !vanished);
        }
        ImGui::SetItemTooltip(
            "%s", Text::GetCString(vanished ? ID::kPlayerMenuUnvanishHint : ID::kPlayerMenuVanishHint));
    }
}
