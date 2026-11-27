#pragma once

#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <string>

namespace PoseWindow
{
    // Any thread: whether an event a graph accepted is missing from the vanilla behaviors.
    [[nodiscard]] bool IsModdedEvent(const RE::BSFixedString& a_eventName);
    // Any thread: an input event the local player's graph accepted, with IsModdedEvent's
    // reading of it.
    void ObserveLocalGraphEvent(const RE::BSFixedString& a_eventName, bool a_modded);

    // Opens a window on a modded event, a reaction (kill move, knockdown, stagger), or a
    // hold; returns whether a window opened this pass.
    bool Update(const LocalFrame& a_local, bool a_transformed, bool a_graphEpochChanged, float a_delta);
    void Discard();

    // Any thread: 0 when the hold limit is reached.
    [[nodiscard]] std::uint32_t AcquireHold(std::string a_owner);
    // Any thread.
    void ReleaseHold(std::uint32_t a_hold);
    void ReleaseHolds(const char* a_reason);

    [[nodiscard]] bool IsOpen();
}
