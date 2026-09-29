#pragma once

#include "protocol/FormReference.h"
#include "sync/SyncFrame.h"

#include <RE/Skyrim.h>

#include <cstdint>

struct RemoteActor;

namespace FurnitureSync
{
    struct RemoteState
    {
        // The latest checkpoint's furniture in the local load order, zero for
        // none or when its plugin is not loaded here.
        RE::FormID refID{ 0 };
    };

    // Local stage, called by CheckpointPublisher once per published checkpoint.
    // Empty for none, and for a reference created at runtime, which peers
    // cannot resolve.
    [[nodiscard]] Multiplayer::FormReference SampleLocalFurniture(const LocalFrame& a_local, std::uint64_t a_sequence);

    // Local stage: whether the local player occupies or has reserved a marker on
    // the furniture, which means its activation was accepted.
    [[nodiscard]] bool IsHeldByLocalPlayer(const LocalFrame& a_local, RE::TESObjectREFR& a_furniture);

    // Local stage, after the local player's furniture activation: whether the
    // actor holds furniture which has no marker left for the local player. Such
    // an actor is hidden so the activation can succeed; each peer keeps its own
    // world, so its source stays seated.
    [[nodiscard]] bool BlocksLocalActivation(
        const LocalFrame& a_local, RE::TESObjectREFR& a_furniture, RemoteActor& a_actor);

    // Remote stage: why the actor must be hidden instead of entering the
    // checkpoint's furniture, or null. The local player occupies it and no other
    // marker is free for the actor; native entry onto the local player's marker
    // is taken back on the next pass. Or another remote player or NPC already
    // holds it, so whoever enters later yields rather than stacking on the same
    // marker.
    [[nodiscard]] const char* FindFurnitureYieldReason(const LocalFrame& a_local, const RemoteFrame& a_remote,
        RE::TESObjectREFR* a_localFurniture, RemoteActor* a_actor);

    // Remote stage, while an actor is hidden for the furniture: whether the local
    // player left it and a marker is free again, so the actor can return.
    [[nodiscard]] bool IsVacated(const LocalFrame& a_local, RE::FormID a_furnitureRefID);

    // Remote stage, after the actor is spawned: a source already using furniture
    // finished its entry before the actor existed, so the actor resumes seated.
    void OnActorSpawned(const RemoteFrame& a_remote, RemoteActor& a_actor);

    // Receive stage: resolves a newer checkpoint's furniture target by plugin
    // name, so peers with different load orders agree on the reference.
    void OnCheckpointAccepted(RemoteState& a_state, const CheckpointUpdate& a_update);

    // Remote stage, before animation staging: applies an exit or a direct
    // furniture-to-furniture change immediately. Returns whether native furniture
    // owns the actor's graph and transform.
    [[nodiscard]] bool SyncFurniture(const RemoteFrame& a_remote, RemoteActor& a_actor);

    // Remote stage, after animation staging: starts the first entry once the
    // checkpoint's grounded locomotion and weapon presentation have settled.
    // Returns the updated furniture ownership result.
    [[nodiscard]] bool SyncFurnitureEntry(
        const RemoteFrame& a_remote, RemoteActor& a_actor, bool a_attackPlaying, bool a_furniturePlaying);
}
