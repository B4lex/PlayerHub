#pragma once

namespace RemoteActorActivation
{
    // Observes the local player activating a remote actor or its mount, so the
    // next pass opens that player's menu. Remote actors block activation, but
    // TESObjectREFR::ActivateRef sends TESActivateEvent before it checks the
    // block. Once data is loaded.
    void RegisterEventSink();
}
