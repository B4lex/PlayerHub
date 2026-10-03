#pragma once

#include "protocol/ChatMessage.h"

// What a player's menu offers beyond whispering: muting the player's lines
// and vanishing its character, or undoing either, for the line menu and the
// player list alike.
namespace UI::ChatPlayerMenu
{
    // In the current popup, after a separator; nothing for the local player
    // or a player without a session.
    void DrawFilterItems(const Multiplayer::RosterEntry& a_player);
}
