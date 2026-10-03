#pragma once

#include "protocol/Session.h"

#include <string>
#include <string_view>
#include <vector>

// The players the local player muted or vanished, per server, by session, in
// skyrim-mp.players next to skyrim-mp.ini. A session ID means something only
// on the server that issued it, which is what the server keys them by.
//
// Every function may read or write the file. Safe from any thread.
namespace Network::PlayerFilterStore
{
    struct Entry
    {
        Multiplayer::SessionID session{ 0 };
        // As the player was last seen, to list it while it is offline.
        std::string name;
        bool muted{ false };
        bool vanished{ false };
    };

    // Appends the entries kept for a_server, the WebSocket URL a connection
    // is opened to.
    void Copy(std::string_view a_server, std::vector<Entry>& a_entries);

    // Appends the sessions vanished on a_server.
    void CopyVanished(std::string_view a_server, std::vector<Multiplayer::SessionID>& a_sessions);

    // Replaces the entry of a_entry.session on a_server, or removes it when it
    // neither mutes nor vanishes. Returns false if the file could not be
    // written; the change still lasts until the game exits.
    bool Save(std::string_view a_server, const Entry& a_entry);
}
