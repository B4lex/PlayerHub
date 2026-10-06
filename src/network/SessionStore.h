#pragma once

#include "protocol/Session.h"

#include <optional>
#include <string_view>

namespace Network::SessionStore
{
    [[nodiscard]] std::optional<Multiplayer::SessionToken> Find(std::string_view a_server);

    bool Save(std::string_view a_server, const Multiplayer::SessionToken& a_token);
}
