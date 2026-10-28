#pragma once

#include "protocol/AppearanceState.h"
#include "protocol/Shapeshift.h"
#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <optional>
#include <string>

namespace AppearanceSync
{
    void SyncAppearance(const LocalFrame& a_local, bool a_transformed, float a_delta);

    struct RemoteState
    {
        std::optional<Multiplayer::PlayerAppearance> latest;
        // A retired reference may still point at its base for a few frames.
        RE::TESNPC* base{ nullptr };
        std::uint64_t baseRevision{ 0 };
        std::optional<std::uint64_t> failedRevision;
        RE::TESNPC* beastBase{ nullptr };
        Multiplayer::Shapeshift beastBaseForm{ Multiplayer::Shapeshift::kNone };
        std::uint64_t beastBaseRevision{ 0 };
        // A form whose race this game cannot resolve spawns as the player's own look.
        Multiplayer::Shapeshift failedForm{ Multiplayer::Shapeshift::kNone };
        std::uint64_t spawnedRevision{ 0 };
        RE::TESNPC* spawnedBase{ nullptr };
        Multiplayer::Shapeshift spawnedForm{ Multiplayer::Shapeshift::kNone };
        float appearanceWaitElapsed{ 0.0F };
        float respawnCooldown{ 0.0F };
        float shapeshiftCooldown{ 0.0F };
        bool colorsApplied{ false };
        RE::NiPointer<RE::NiSourceTexture> faceTintTexture;
    };

    [[nodiscard]] std::string GetRaceName(Multiplayer::PeerSlot a_playerID,
        const Multiplayer::AppearanceFormReference& a_race,
        const std::optional<Multiplayer::AppearanceFormReference>& a_defaultRace);

    void AcceptRemoteAppearance(
        RemoteState& a_remote, Multiplayer::PeerSlot a_playerID, Multiplayer::PlayerAppearance&& a_appearance);

    void SyncAppearance(RemoteState& a_remote, float a_delta);

    struct SpawnBase
    {
        RE::TESNPC* base{ nullptr };
        Multiplayer::Shapeshift form{ Multiplayer::Shapeshift::kNone };
    };

    [[nodiscard]] SpawnBase GetSpawnBase(
        RemoteState& a_remote, Multiplayer::PeerSlot a_playerID, Multiplayer::Shapeshift a_form);

    void OnActorSpawned(RemoteState& a_remote, const SpawnBase& a_spawned);

    [[nodiscard]] bool RequiresRespawn(const RemoteState& a_remote);
    [[nodiscard]] bool RequiresShapeshift(const RemoteState& a_remote, Multiplayer::Shapeshift a_form);

    void ApplyActorPresentation(
        RemoteState& a_remote, Multiplayer::PeerSlot a_playerID, RE::Actor& a_actor, bool a_actorReady);
}
