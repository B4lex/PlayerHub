#pragma once

#include "sync/SyncFrame.h"

// Whether the local player is placed in the game world, as opposed to the main
// menu or a loading screen. Peers keep a remote actor only while its player is
// in the world, and the server accepts checkpoints only then. Also starts and
// ends the GameSession.
namespace PresenceSync
{
    // Registers the menu sink that reports leaving the world as soon as a
    // loading screen or the main menu opens, before the main loop may stall on
    // the load. Requires Skyrim's data-loaded state.
    void RegisterEventSinks();

    // Called when a save starts loading, before the engine tears down the world.
    void OnGameLoadStarted();

    // Local frame stage, before any other local publication, so player:enter
    // precedes the checkpoint it admits. Publishes the transitions, including a
    // new player:enter after every reconnect, and returns whether the local
    // player is in the world.
    [[nodiscard]] bool SyncPresence(const LocalFrame& a_local);
}
