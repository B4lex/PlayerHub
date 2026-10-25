#pragma once

namespace RemoteActorActivation
{
    void InstallHooks();
    // TESObjectREFR::ActivateRef sends TESActivateEvent before it checks whether
    // activation is blocked.
    void RegisterEventSink();
}
