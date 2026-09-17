#pragma once

#include <RE/Skyrim.h>

struct RemoteActor;

namespace AnimationFirewall
{
    // Installs the local animation-event filter and the Direction channel
    // override for registered remote actors.
    void InstallHooks();

    // Admits only network-owned and client-managed events once enabled.
    void Enable(RemoteActor& a_remote);
    void Disable(RemoteActor& a_remote);

    // Observes graph output annotations (landing and weapon-transition ends).
    [[nodiscard]] bool RegisterGraphOutputEventSink(RE::BSAnimationGraphManager& a_graphManager);

    // Delivers a network-owned event past the filter. Game thread only.
    [[nodiscard]] bool NotifyNetworkAnimationGraph(RE::Actor& a_actor, const RE::BSFixedString& a_eventName);
}
