#pragma once

#include "protocol/Session.h"

#include <optional>
#include <string_view>

// The tokens of the player's sessions, one per server, in skyrim-mp.session
// next to skyrim-mp.ini. They are kept out of the ini because players share
// that file, and a token is all it takes to connect as its owner.
//
// Both functions may read or write the file, so call them off the game thread.
// Safe from any thread.
namespace Network::SessionStore
{
    // The token of the session with a_server, the WebSocket URL a connection
    // is opened to. Empty when there is none, so the server issues one.
    [[nodiscard]] std::optional<Multiplayer::SessionToken> Find(std::string_view a_server);

    // Keeps the token the server issued, replacing any earlier one for that
    // server. Returns false if the file could not be written; the token is
    // still used until the game exits.
    bool Save(std::string_view a_server, const Multiplayer::SessionToken& a_token);
}
