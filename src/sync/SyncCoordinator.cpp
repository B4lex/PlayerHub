#include "sync/SyncCoordinator.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorRuntime.h"
#include "network/SocketIOClient.h"
#include "runtime/GameThread.h"
#include "sync/ActorLifecycle.h"
#include "sync/CheckpointPublisher.h"
#include "sync/PeerState.h"
#include "sync/SyncFrame.h"
#include "sync/animation/AnimationSync.h"
#include "sync/appearance/AppearanceSync.h"
#include "sync/combat/CombatSync.h"
#include "sync/death/DeathSync.h"
#include "sync/equipment/EquipmentSync.h"
#include "sync/furniture/FurnitureSync.h"
#include "sync/mount/MountSync.h"
#include "sync/movement/MovementSync.h"
#include "sync/presence/PresenceSync.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace SyncCoordinator
{
    namespace
    {
        // Accessed only on the bound main-loop thread.
        Peers s_peers;
        std::uint64_t s_peersConnectionGeneration = 0;
        bool s_actorsExcludedFromSaves = false;

        struct LocalFurnitureActivation
        {
            RE::FormID furnitureRefID{ 0 };
            bool deferred{ false };
        };

        // Skyrim activates on a worker thread, so activations are queued there and
        // handled by the next main-loop pass.
        std::mutex s_localFurnitureActivationsLock;
        std::vector<LocalFurnitureActivation> s_localFurnitureActivations;

        // Remote player IDs are the server's per-connection sids, so a closed or
        // replaced connection leaves no peer that the server will update or
        // report as left. Reconnecting rebuilds every peer from resent state.
        void RemovePeersFromLostConnection(Peers& a_peers, const Network::RemotePlayerChanges& a_changes)
        {
            if (a_changes.connected && a_changes.connectionGeneration == s_peersConnectionGeneration)
            {
                return;
            }
            if (a_changes.connected)
            {
                s_peersConnectionGeneration = a_changes.connectionGeneration;
            }
            if (a_peers.empty())
            {
                return;
            }

            for (auto& [playerID, peer] : a_peers)
            {
                ActorLifecycle::RemoveActor(playerID, peer);
            }
            logs::debug(
                "Removed {} remote player actors after losing the server connection: connected={} "
                "connectionGeneration={}",
                a_peers.size(), a_changes.connected, static_cast<unsigned long long>(a_changes.connectionGeneration));
            a_peers.clear();
            if (auto* console = RE::ConsoleLog::GetSingleton())
            {
                console->Print("MP: Server connection lost; removed remote players.");
            }
        }

        // The player left the world. Only its appearance and equipment stay valid;
        // its next checkpoint places it again as if it had just joined.
        void ForgetWorldState(const std::string& a_playerID, PeerState& a_peer)
        {
            ActorLifecycle::RemoveActor(a_playerID, a_peer);
            a_peer.lifecycle = {};
            a_peer.combat = {};
            a_peer.mounting = {};
            a_peer.latestCheckpoint = {};
            a_peer.lastSequence = 0;
            a_peer.appliedSequence = 0;
        }

        // Receive stage. Transforms are latest-wins by sequence, appearance and
        // equipment by revision; actions are ordered and bounded; a world exit
        // clears the player's checkpoint state and a disconnect clears everything.
        // Exits apply first, since changes drained with one arrived after it.
        void ApplyNetworkChanges(Peers& a_peers)
        {
            auto changes = Network::DrainRemotePlayerChanges();
            RemovePeersFromLostConnection(a_peers, changes);

            for (const auto& playerID : changes.exitedPlayerIDs)
            {
                const auto peer = a_peers.find(playerID);
                if (peer == a_peers.end())
                {
                    continue;
                }

                const bool hadActor = peer->second.lifecycle.actor.has_value();
                ForgetWorldState(playerID, peer->second);
                logs::debug("Removed remote player {} from the world after server player:exit: hadActor={}", playerID,
                    hadActor);
            }

            for (const auto& playerID : changes.disconnectedPlayerIDs)
            {
                const auto peer = a_peers.find(playerID);
                if (peer == a_peers.end())
                {
                    continue;
                }

                ActorLifecycle::RemoveActor(playerID, peer->second);
                a_peers.erase(peer);
                logs::debug("Removed remote player actor {} after server player:disconnect", playerID);
            }

            for (auto& update : changes.transforms)
            {
                auto [peer, inserted] = a_peers.try_emplace(update.playerID);
                auto& state = peer->second;
                if (!inserted && update.transform.sequence <= state.lastSequence)
                {
                    logs::debug(
                        "Animation sync ignored stale main-loop snapshot: player={} sequence={} state={} "
                        "newestSequence={} newestState={}",
                        update.playerID, static_cast<unsigned long long>(update.transform.sequence),
                        Multiplayer::GetMovementStateName(update.transform.animationState.movementState),
                        static_cast<unsigned long long>(state.lastSequence),
                        Multiplayer::GetMovementStateName(state.latestCheckpoint.animationState.movementState));
                    continue;
                }

                const CheckpointUpdate checkpointUpdate{ .playerID = update.playerID,
                    .inserted = inserted,
                    .previous = state.latestCheckpoint,
                    .previousSequence = state.lastSequence,
                    .checkpoint = update.transform };
                FurnitureSync::OnCheckpointAccepted(checkpointUpdate);
                AnimationSync::OnCheckpointAccepted(checkpointUpdate);
                CombatSync::OnCheckpointAccepted(state.combat, checkpointUpdate);
                state.latestCheckpoint = update.transform;
                state.lastSequence = update.transform.sequence;
            }

            for (auto& update : changes.appearances)
            {
                // An appearance may arrive before the first checkpoint. The actor is
                // only spawned once a checkpoint places it in the local space.
                auto& state = a_peers[update.playerID];
                AppearanceSync::AcceptRemoteAppearance(state.appearance, update.playerID, std::move(update.appearance));
            }

            for (auto& update : changes.equipment)
            {
                // Equipment is kept independent of whether an actor reference exists,
                // so every respawn reconciles from the newest revision.
                auto& state = a_peers[update.playerID];
                EquipmentSync::AcceptRemoteEquipment(state.equipment, update.playerID, std::move(update.equipment));
            }

            for (auto& action : changes.animationActions)
            {
                const auto peer = a_peers.find(action.playerID);
                const bool known = peer != a_peers.end();
                CombatSync::AcceptRemoteAction(known ? &peer->second.combat : nullptr,
                    known ? &peer->second.latestCheckpoint : nullptr, std::move(action));
            }

            for (const auto& transition : changes.mountTransitions)
            {
                const auto peer = a_peers.find(transition.playerID);
                const bool known = peer != a_peers.end();
                MountSync::AcceptRemoteTransition(known ? &peer->second.mounting : nullptr,
                    known ? &peer->second.latestCheckpoint : nullptr, known ? peer->second.lastSequence : 0,
                    transition);
            }
        }

        // Hides remote actors that left an activated furniture no free marker, then
        // activates it again for the local player when that hid an actor or the
        // original activation was deferred.
        void YieldActivatedFurniture(Peers& a_peers, RE::PlayerCharacter& a_player)
        {
            std::vector<LocalFurnitureActivation> activations;
            {
                const std::scoped_lock lock{ s_localFurnitureActivationsLock };
                activations.swap(s_localFurnitureActivations);
            }

            const LocalFrame local{ .player = a_player };
            for (const auto& activation : activations)
            {
                auto* furniture = RE::TESForm::LookupByID<RE::TESObjectREFR>(activation.furnitureRefID);
                const bool held = furniture && FurnitureSync::IsHeldByLocalPlayer(local, *furniture);
                logs::debug("Observed local furniture activation: furniture={:08X} resolved={} deferred={} held={}",
                    activation.furnitureRefID, furniture != nullptr, activation.deferred, held);
                if (!furniture || held)
                {
                    continue;
                }

                std::size_t yielded = 0;
                for (auto& [playerID, peer] : a_peers)
                {
                    if (!peer.lifecycle.actor
                        || !FurnitureSync::BlocksLocalActivation(local, *furniture, *peer.lifecycle.actor))
                    {
                        continue;
                    }
                    const RemoteFrame remote{
                        .playerID = playerID, .checkpoint = peer.latestCheckpoint, .lastSequence = peer.lastSequence
                    };
                    ActorLifecycle::YieldFurniture(remote, peer, activation.furnitureRefID, "local activation");
                    ++yielded;
                }
                if (yielded == 0 && !activation.deferred)
                {
                    continue;
                }

                // ActivateRef already ran the reference's scripts; only the furniture's
                // own activation is repeated.
                const bool reactivated = furniture->ActivateRef(&a_player, 0, nullptr, 1, true);
                logs::debug(
                    "Reactivated furniture for the local player after hiding {} remote actors: furniture={:08X} "
                    "deferred={} reactivated={}",
                    yielded, activation.furnitureRefID, activation.deferred, reactivated);
            }
        }

        void RestoreSaveEligibility(Peers& a_peers)
        {
            if (!s_actorsExcludedFromSaves)
            {
                return;
            }
            auto* saveLoadGame = RE::BGSSaveLoadGame::GetSingleton();
            if (saveLoadGame && saveLoadGame->GetSaveGameSaving())
            {
                return;
            }

            // The flag is only meant for the save writer; its other engine uses
            // concern references pending destruction, so it is not kept longer.
            for (auto& [playerID, peer] : a_peers)
            {
                ActorLifecycle::SetExcludedFromSaves(peer, false);
            }
            ActorLifecycle::SetDeferredRemovalsExcludedFromSaves(false);
            s_actorsExcludedFromSaves = false;
        }

        // Local stages. Attack capture and graph-epoch observation run every pass;
        // the checkpoint and its attacks are published at the snapshot cadence.
        void SyncLocalPlayer(RE::PlayerCharacter& a_player, float a_delta)
        {
            const LocalFrame local{ .player = a_player };

            const auto capturedAttack = CombatSync::CaptureLocalAttack(local);
            const bool handPresentationStarted = CombatSync::CaptureLocalHandPresentation(local);
            const auto graphEpoch = AnimationSync::ObserveLocalGraphEpoch(local);
            // Flush equipment before a captured attack, bow draw, cast, or block can
            // be published, so peers receive a weapon swap ahead of its use.
            EquipmentSync::SyncEquipment(local, a_delta, capturedAttack.has_value() || handPresentationStarted);
            CombatSync::QueueLocalAttacks(capturedAttack, graphEpoch.changed);
            const auto publication = CheckpointPublisher::PublishLocalCheckpoint(local, a_delta, graphEpoch.epoch);
            switch (publication.status)
            {
                case CheckpointPublisher::Status::kDisconnected:
                    CombatSync::DiscardLocalAttacks();
                    break;
                case CheckpointPublisher::Status::kPublished:
                    // player:update always precedes the animation:action and
                    // mount:transition it anchors.
                    CombatSync::PublishLocalAttacks(publication.checkpoint);
                    MountSync::PublishLocalTransition(publication.checkpoint);
                    break;
                default:
                    break;
            }
            AppearanceSync::SyncAppearance(local, a_delta);
        }

        // Remote stages for one peer.
        void SyncPeer(const std::string& a_playerID, PeerState& a_peer, const LocalFrame& a_local,
            RE::FormID a_localCellID, RE::FormID a_localWorldspaceID, RE::TESObjectREFR* a_localFurniture,
            float a_delta)
        {
            const RemoteFrame remote{
                .playerID = a_playerID, .checkpoint = a_peer.latestCheckpoint, .lastSequence = a_peer.lastSequence
            };
            auto& lifecycle = a_peer.lifecycle;

            const bool isInLocalSpace = MovementSync::IsInLocalSpace(remote, a_localCellID, a_localWorldspaceID);
            const bool changedSpawnedSpace = ActorLifecycle::HasChangedSpawnedSpace(a_peer);
            AppearanceSync::SyncAppearance(a_peer.appearance, a_delta);
            if (lifecycle.actor && isInLocalSpace && !changedSpawnedSpace
                && AppearanceSync::RequiresRespawn(a_peer.appearance))
            {
                ActorLifecycle::RetireForAppearanceChange(remote, a_peer);
            }
            if (lifecycle.actor && (!isInLocalSpace || changedSpawnedSpace))
            {
                ActorLifecycle::RetireForSpaceChange(remote, a_peer, a_localCellID, a_localWorldspaceID);
            }

            CombatSync::AgeRemoteActions(remote, a_peer.combat, a_delta);
            if (!isInLocalSpace)
            {
                // Do not preserve transient combat history while no actor/checkpoint
                // can be reconstructed in the local coordinate space.
                CombatSync::ClearRemoteActions(a_peer.combat);
                return;
            }
            if (lifecycle.actor && DeathSync::SyncDeath(remote, a_peer))
            {
                // The body is left to Skyrim's ragdoll; only its handle and look are kept.
                auto& actor = *lifecycle.actor;
                if (auto liveActor = actor.handle.get())
                {
                    AppearanceSync::ApplyActorPresentation(a_peer.appearance, a_playerID, *liveActor, actor.ready);
                }
                else
                {
                    ActorLifecycle::OnActorLost(remote, a_peer);
                }
                return;
            }
            if (lifecycle.actor)
            {
                // Same-space fast travel keeps the coordinate space, so it is
                // detected by displacement instead. The actor respawns below.
                ActorLifecycle::RetireWhenStranded(remote, a_peer, a_delta);
            }
            if (lifecycle.yieldedFurnitureRefID != remote.checkpoint.furnitureRefID)
            {
                if (const auto* reason = FurnitureSync::FindFurnitureYieldReason(
                        a_local, remote, a_localFurniture, lifecycle.actor ? &*lifecycle.actor : nullptr))
                {
                    ActorLifecycle::YieldFurniture(remote, a_peer, remote.checkpoint.furnitureRefID, reason);
                }
            }

            if (lifecycle.actor && MountSync::SyncMount(remote, a_peer, a_delta))
            {
                // The mount owns the actor: it carries it, or plays its entry or
                // dismount. Only its equipment and look are kept.
                auto& actor = *lifecycle.actor;
                CombatSync::ClearRemoteActions(a_peer.combat);
                EquipmentSync::SyncEquipment(a_peer.equipment, a_playerID, actor, a_delta);
                if (!MountSync::UpdateRider(a_peer, a_delta))
                {
                    ActorLifecycle::OnActorLost(remote, a_peer);
                }
                else if (auto liveActor = actor.handle.get())
                {
                    AppearanceSync::ApplyActorPresentation(a_peer.appearance, a_playerID, *liveActor, actor.ready);
                }
            }
            else if (lifecycle.actor)
            {
                auto& actor = *lifecycle.actor;
                MovementSync::SyncMovement(remote, a_peer.appliedSequence, actor);
                EquipmentSync::SyncEquipment(a_peer.equipment, a_playerID, actor, a_delta);
                const bool handsSettled = EquipmentSync::AreHandsSettled(a_peer.equipment);
                const bool readyForWeaponDraw = EquipmentSync::IsReadyForWeaponDraw(a_peer.equipment);

                bool furniturePlaying = FurnitureSync::SyncFurniture(remote, actor);
                const bool attackPlaying = ActorAnimation::IsAnimationActionPlaying(actor);
                if (!attackPlaying && !furniturePlaying)
                {
                    AnimationSync::SyncAnimation(remote, actor, readyForWeaponDraw);
                }
                furniturePlaying = FurnitureSync::SyncFurnitureEntry(remote, actor, attackPlaying, furniturePlaying);
                // Stage the freshest base state before considering an action. If this
                // changes the graph request, CanDispatchAttack keeps the action
                // queued until ApplyAnimationUpdate settles it on a following frame.
                if (!furniturePlaying)
                {
                    CombatSync::SyncCombat(remote, a_peer.combat, actor, handsSettled);
                    CombatSync::SyncHandPresentation(remote, actor, handsSettled, a_delta);
                }
                const auto translationState
                    = AnimationSync::GetTranslationState(remote, actor, readyForWeaponDraw, furniturePlaying);

                if (!ActorRuntime::Update(actor, a_delta, translationState))
                {
                    ActorLifecycle::OnActorLost(remote, a_peer);
                }
                else if (auto liveActor = actor.handle.get())
                {
                    AppearanceSync::ApplyActorPresentation(a_peer.appearance, a_playerID, *liveActor, actor.ready);
                }
            }

            if (lifecycle.actor)
            {
                return;
            }
            if (lifecycle.yieldedFurnitureRefID != 0
                && lifecycle.yieldedFurnitureRefID == remote.checkpoint.furnitureRefID
                && FurnitureSync::IsVacated(a_local, lifecycle.yieldedFurnitureRefID))
            {
                ActorLifecycle::EndFurnitureYield(remote, a_peer);
            }
            ActorLifecycle::SpawnWhenDue(remote, a_peer, a_delta);
        }
    }

    void OnFrame(RE::PlayerCharacter* a_player, float a_delta)
    {
        RestoreSaveEligibility(s_peers);
        ApplyNetworkChanges(s_peers);
        ActorLifecycle::UpdateDeferredRemovals(a_delta);
        if (!a_player)
        {
            return;
        }

        YieldActivatedFurniture(s_peers, *a_player);
        if (PresenceSync::SyncPresence(LocalFrame{ .player = *a_player }))
        {
            SyncLocalPlayer(*a_player, a_delta);
        }
        else
        {
            // Attacks captured before leaving would otherwise anchor to the first
            // checkpoint after returning.
            CombatSync::DiscardLocalAttacks();
        }

        auto* localCell = a_player->GetParentCell();
        if (!localCell)
        {
            return;
        }

        const LocalFrame local{ .player = *a_player };
        const auto localFurniture = a_player->GetOccupiedFurniture().get();
        for (auto& [playerID, peer] : s_peers)
        {
            SyncPeer(playerID, peer, local, localCell->GetFormID(), MovementSync::GetWorldspaceID(*a_player),
                localFurniture.get(), a_delta);
        }
    }

    void QueueLocalFurnitureActivation(RE::FormID a_furnitureRefID, bool a_deferred)
    {
        const std::scoped_lock lock{ s_localFurnitureActivationsLock };
        s_localFurnitureActivations.push_back({ .furnitureRefID = a_furnitureRefID, .deferred = a_deferred });
    }

    void OnGameSave()
    {
        if (!GameThread::Require("ExcludeRemoteActorsFromSave"))
        {
            // Touching peers here would race the main loop; the save keeps actors.
            return;
        }

        std::size_t excluded = 0;
        for (auto& [playerID, peer] : s_peers)
        {
            if (peer.lifecycle.actor)
            {
                ActorLifecycle::SetExcludedFromSaves(peer, true);
                ++excluded;
            }
        }
        ActorLifecycle::SetDeferredRemovalsExcludedFromSaves(true);
        // Every peer is restored, so an actor spawned after this is unaffected.
        s_actorsExcludedFromSaves = true;
        logs::debug("Excluded {} remote player actors from game save", excluded);
    }
}
