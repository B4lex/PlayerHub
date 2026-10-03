#pragma once

#include "protocol/ChatMessage.h"

// What a player's menu offers beyond whispering, for the line menu, the
// player list, and the menu that activating a player's character opens alike.
namespace UI::ChatPlayerMenu
{
    // Muting the player's lines and vanishing its character, or undoing
    // either, in the current popup after a separator; nothing for the local
    // player or a player without a session.
    void DrawFilterItems(const Multiplayer::RosterEntry& a_player);
}
