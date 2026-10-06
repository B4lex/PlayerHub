#pragma once

#include "sync/SyncFrame.h"

namespace PresenceSync
{
    void RegisterEventSinks();

    void OnGameLoadStarted();

    [[nodiscard]] bool SyncPresence(const LocalFrame& a_local);
}
