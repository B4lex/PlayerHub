#pragma once

#include <RE/Skyrim.h>

namespace SyncCoordinator
{
    void OnFrame(RE::PlayerCharacter* a_player, float a_delta);

    void OnGameSave();

    void QueueLocalFurnitureActivation(RE::FormID a_furnitureRefID, bool a_deferred);

    void QueueLocalActorActivation(RE::FormID a_actorID);
}
