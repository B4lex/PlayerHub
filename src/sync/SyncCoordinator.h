#pragma once

#include <RE/Skyrim.h>

namespace SyncCoordinator
{
    // The single synchronization entry point, called once per main-loop update
    // on the bound game thread. Drains network changes once, then runs the local
    // and remote stages in a fixed order.
    void OnFrame(RE::PlayerCharacter* a_player, float a_delta);

    // Called before Skyrim writes a save. Remote actors stay in the world but are
    // left out of the save, so none is restored from it; the next pass after
    // saving finishes makes them saveable engine references again.
    void OnGameSave();

    // Thread-safe; called when the local player activates furniture. The next
    // pass hides remote actors that leave it no free marker, then activates it
    // again when that hid an actor or Skyrim's activation was deferred.
    void QueueLocalFurnitureActivation(RE::FormID a_furnitureRefID, bool a_deferred);

    // Thread-safe; called when the local player activates a remote actor or its
    // mount. The next pass opens that player's menu.
    void QueueLocalActorActivation(RE::FormID a_actorID);
}
