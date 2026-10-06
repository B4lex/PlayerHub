#pragma once

#include "protocol/AppearanceState.h"
#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <optional>
#include <string>

namespace AppearanceSync
{
    void SyncAppearance(const LocalFrame& a_local, float a_delta);

    struct RemoteState
    {
        std::optional<Multiplayer::PlayerAppearance> latest;
        // A retired reference may still point at its base for a few frames.
        RE::TESNPC* base{ nullptr };
        std::uint64_t baseRevision{ 0 };
        std::optional<std::uint64_t> failedRevision;
        std::uint64_t spawnedRevision{ 0 };
        RE::TESNPC* spawnedBase{ nullptr };
        float appearanceWaitElapsed{ 0.0F };
        float respawnCooldown{ 0.0F };
        bool colorsApplied{ false };
        RE::NiPointer<RE::NiSourceTexture> faceTintTexture;
    };

    void AcceptRemoteAppearance(
        RemoteState& a_remote, Multiplayer::PeerSlot a_playerID, Multiplayer::PlayerAppearance&& a_appearance);

    void SyncAppearance(RemoteState& a_remote, float a_delta);

    [[nodiscard]] RE::TESNPC* GetSpawnBase(RemoteState& a_remote, Multiplayer::PeerSlot a_playerID);

    void OnActorSpawned(RemoteState& a_remote, RE::TESNPC* a_spawnedBase);

    [[nodiscard]] bool RequiresRespawn(const RemoteState& a_remote);

    void ApplyActorPresentation(
        RemoteState& a_remote, Multiplayer::PeerSlot a_playerID, RE::Actor& a_actor, bool a_actorReady);
}
