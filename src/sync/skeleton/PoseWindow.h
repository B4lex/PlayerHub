#pragma once

#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

namespace PoseWindow
{
    // Any thread: whether the local player's third-person graph lists the event past
    // the vanilla events. False while that graph lacks the vanilla prefix.
    [[nodiscard]] bool IsModdedEvent(const RE::BSFixedString& a_eventName);
    // Any thread: an input event the local player's graph accepted, with IsModdedEvent's
    // reading of it.
    void ObserveLocalGraphEvent(const RE::BSFixedString& a_eventName, bool a_modded);

    // Opens a window on a modded event or on a reaction (kill move, knockdown, stagger);
    // returns whether a window opened this pass.
    bool Update(const LocalFrame& a_local, bool a_transformed, bool a_graphEpochChanged, float a_delta);
    void Discard();
    [[nodiscard]] bool IsOpen();
}
