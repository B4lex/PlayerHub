#pragma once

#ifndef NDEBUG
#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

namespace GraphEventSurvey
{
    // Called by the player's graph input hook on any thread.
    void ObserveLocalGraphEvent(const RE::BSFixedString& a_eventName);

    void LogLocalGraphEvents(const LocalFrame& a_local, float a_delta);
}
#endif
