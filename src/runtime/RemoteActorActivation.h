#pragma once

namespace RemoteActorActivation
{
    // TESObjectREFR::ActivateRef sends TESActivateEvent before it checks whether
    // activation is blocked.
    void RegisterEventSink();
}
