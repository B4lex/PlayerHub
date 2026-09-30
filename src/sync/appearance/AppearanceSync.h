#pragma once

#include "protocol/AppearanceState.h"
#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <optional>
#include <string>

namespace AppearanceSync
{
    // Captures the local player's appearance about once per second and publishes
    // debounced, rate-limited revisions. Re-publishes after every reconnect.
    // Local frame stage. Must run on the bound main-loop thread.
    void SyncAppearance(const LocalFrame& a_local, float a_delta);

    // Per remote player receive state. Accessed only by the main-loop pass.
    struct RemoteState
    {
        std::optional<Multiplayer::PlayerAppearance> latest;
        // Runtime actor base built for baseRevision, where zero is the default look.
        // Bases are never freed: a retired reference may still point at its base
        // for a few frames.
        RE::TESNPC* base{ nullptr };
        std::uint64_t baseRevision{ 0 };
        std::optional<std::uint64_t> failedRevision;
        // Revision the current remote actor was spawned with; zero for the default look.
        std::uint64_t spawnedRevision{ 0 };
        RE::TESNPC* spawnedBase{ nullptr };
        float appearanceWaitElapsed{ 0.0F };
        float respawnCooldown{ 0.0F };
        bool colorsApplied{ false };
        // Face tint texture the spawned remote actor's tints were last rendered into.
        RE::NiPointer<RE::NiSourceTexture> faceTintTexture;
    };

    void AcceptRemoteAppearance(
        RemoteState& a_remote, Multiplayer::PeerSlot a_playerID, Multiplayer::PlayerAppearance&& a_appearance);

    // Remote frame stage: advances first-appearance wait and respawn cooldown.
    void SyncAppearance(RemoteState& a_remote, float a_delta);

    // Returns the actor base a new remote actor should use, building a fresh runtime
    // base for the latest revision if needed. Returns nullptr while a first
    // appearance is still expected; afterwards builds a default look. A revision
    // that cannot be built keeps the previously built base, if any.
    [[nodiscard]] RE::TESNPC* GetSpawnBase(RemoteState& a_remote, Multiplayer::PeerSlot a_playerID);

    void OnActorSpawned(RemoteState& a_remote, RE::TESNPC* a_spawnedBase);

    // True when a newer appearance than the spawned one should replace the remote actor.
    [[nodiscard]] bool RequiresRespawn(const RemoteState& a_remote);

    // Applies post-3D presentation: hair and skin color once per spawned actor, and
    // the face tint render whenever the actor's head gets a new tint texture.
    void ApplyActorPresentation(
        RemoteState& a_remote, Multiplayer::PeerSlot a_playerID, RE::Actor& a_actor, bool a_actorReady);
}
