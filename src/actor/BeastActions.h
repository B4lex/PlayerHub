#pragma once

#include "protocol/Shapeshift.h"

#include <RE/Skyrim.h>

#include <optional>

namespace BeastActions
{
    [[nodiscard]] const RE::BSFixedString& GetEventName(Multiplayer::BeastAction a_action);

    // Any thread. The action of a graph input event, if the beast graph's vocabulary has it.
    [[nodiscard]] std::optional<Multiplayer::BeastAction> Find(
        Multiplayer::Shapeshift a_graph, const RE::BSFixedString& a_eventName);
}
