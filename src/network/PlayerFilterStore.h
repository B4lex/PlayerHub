#pragma once

#include "protocol/Session.h"

#include <string>
#include <string_view>
#include <vector>

namespace Network::PlayerFilterStore
{
    struct Entry
    {
        Multiplayer::SessionID session{ 0 };
        std::string name;
        bool friended{ false };
        bool muted{ false };
        bool vanished{ false };
    };

    void Copy(std::string_view a_server, std::vector<Entry>& a_entries);

    void CopyVanished(std::string_view a_server, std::vector<Multiplayer::SessionID>& a_sessions);

    bool Save(std::string_view a_server, const Entry& a_entry);
}
