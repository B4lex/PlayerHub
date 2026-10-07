#pragma once

#include <RE/Skyrim.h>

namespace PrecisionIsolation
{
    void Initialize();

    void Exclude(RE::ActorHandle a_handle);
    void Release(RE::ActorHandle a_handle);

    void Update(const RE::TESObjectCELL& a_playerCell, float a_deltaTime);
}
