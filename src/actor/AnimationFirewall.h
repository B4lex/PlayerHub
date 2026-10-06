#pragma once

#include <RE/Skyrim.h>

struct RemoteActor;

namespace AnimationFirewall
{
    void InstallHooks();

    void Enable(RemoteActor& a_remote);
    void Disable(RemoteActor& a_remote);

    [[nodiscard]] bool RegisterGraphOutputEventSink(RE::BSAnimationGraphManager& a_graphManager);

    [[nodiscard]] bool NotifyNetworkAnimationGraph(RE::Actor& a_actor, const RE::BSFixedString& a_eventName);

    // IdleForceDefaultState also drops the graph's torch arm layer.
    [[nodiscard]] bool ForceDefaultGraphState(RE::Actor& a_actor);
}
