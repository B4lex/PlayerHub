#pragma once

#include "protocol/EffectState.h"
#include "protocol/PeerSlot.h"
#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <optional>
#include <vector>

struct RemoteActor;

namespace EffectSync
{
    void SyncLocalEffects(const LocalFrame& a_local, float a_delta);

    struct ResolvedEffect
    {
        RE::EffectSetting* effect{ nullptr };
        float magnitude{ 0.0F };
        // Reapplications after the engine ended the effect while the source kept it.
        std::uint32_t reapplies{ 0 };
    };

    struct RemoteState
    {
        struct Held
        {
            Multiplayer::PlayerEffects effects;
            float elapsed{ 0.0F };
        };

        // The effects in force; newer ones wait in held for the peer's playout delay.
        std::optional<Multiplayer::PlayerEffects> latest;
        std::vector<Held> held;
        std::uint64_t resolvedRevision{ 0 };
        bool resolvedShowing{ false };
        bool resolved{ false };
        std::vector<ResolvedEffect> desired;
        // On the current actor, from EffectPresentation.
        std::vector<ResolvedEffect> applied;
        bool dirty{ true };
        float checkElapsed{ 0.0F };
    };

    void AcceptRemoteEffects(RemoteState& a_remote, Multiplayer::PeerSlot a_playerID, Multiplayer::PlayerEffects&& a_effects);

    // Every pass, with or without an actor.
    void AgeRemoteEffects(RemoteState& a_remote, double a_holdSeconds, float a_delta);

    void OnActorSpawned(RemoteState& a_remote);

    void SyncEffects(RemoteState& a_remote, Multiplayer::PeerSlot a_playerID, RemoteActor& a_remoteActor, float a_delta);
}
