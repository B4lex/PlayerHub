#include "sync/SyncCoordinator.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorRuntime.h"
#include "actor/PrecisionIsolation.h"
#include "network/ServerClient.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"
#include "sync/ActorLifecycle.h"
#include "sync/CheckpointPublisher.h"
#include "sync/PeerState.h"
#include "sync/SyncFrame.h"
#include "sync/animation/AnimationSync.h"
#include "sync/appearance/AppearanceSync.h"
#include "sync/chat/PlayerActivation.h"
#include "sync/chat/PlayerDetails.h"
#include "sync/combat/CombatSync.h"
#include "sync/death/DeathSync.h"
#include "sync/equipment/EquipmentSync.h"
#include "sync/furniture/FurnitureSync.h"
#include "sync/mount/MountSync.h"
#include "sync/nameplates/NameplateSync.h"
#include "sync/movement/MovementSync.h"
#include "sync/outfit/DefaultOutfit.h"
#include "sync/presence/PresenceSync.h"
#include "sync/trade/TradeSync.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace SyncCoordinator
{
    namespace
    {
        Peers s_peers;
        std::uint64_t s_peersConnectionGeneration = 0;
        bool s_actorsExcludedFromSaves = false;

        struct LocalFurnitureActivation
        {
            RE::FormID furnitureRefID{ 0 };
            bool deferred{ false };
        };

        // Skyrim activates on a worker thread.
        std::mutex s_localFurnitureActivationsLock;
        std::vector<LocalFurnitureActivation> s_localFurnitureActivations;
        std::atomic<RE::FormID> s_localActorActivation{ 0 };

        RemoteFrame GetRemoteFrame(Multiplayer::PeerSlot a_playerID, const PeerState& a_peer)
        {
            const auto& checkpoint = a_peer.playout.GetPlayed();
            return RemoteFrame{ .playerID = a_playerID,
                .checkpoint = checkpoint,
                .pose = a_peer.playout.GetPose(),
                .furnitureRefID = a_peer.furniture.refID };
        }

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
                ActorLifecycle::RemoveActor(peer);
            }
            logs::debug(
                "Removed {} remote player actors after losing the server connection: connected={} "
                "connectionGeneration={}",
                a_peers.size(), a_changes.connected, static_cast<unsigned long long>(a_changes.connectionGeneration));
            a_peers.clear();
        }

        void ForgetWorldState(PeerState& a_peer)
        {
            ActorLifecycle::RemoveActor(a_peer);
            a_peer.lifecycle = {};
            a_peer.combat = {};
            a_peer.furniture = {};
            a_peer.playout.Reset();
            a_peer.appliedSequence = 0;
        }

        void ApplyNetworkChanges(Peers& a_peers)
        {
            PROFILE_FUNCTION();
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
                ForgetWorldState(peer->second);
                logs::debug("Removed remote player {} from the world after PeerLeave: hadActor={}", playerID,
                    hadActor);
            }

            for (const auto& playerID : changes.disconnectedPlayerIDs)
            {
                const auto peer = a_peers.find(playerID);
                if (peer == a_peers.end())
                {
                    continue;
                }

                ActorLifecycle::RemoveActor(peer->second);
                a_peers.erase(peer);
                logs::debug("Removed remote player actor {} after its disconnect", playerID);
            }

            for (auto& update : changes.transforms)
            {
                auto& playout = a_peers[update.playerID].playout;
                const auto sequence = update.transform.sequence;
                if (!playout.Push(std::move(update.transform), update.received))
                {
                    const auto* newest = playout.GetNewest();
                    logs::debug("Checkpoint playout ignored stale checkpoint: player={} sequence={} newestSequence={}",
                        update.playerID, static_cast<unsigned long long>(sequence),
                        static_cast<unsigned long long>(newest ? newest->sequence : 0));
                }
            }

            for (auto& update : changes.appearances)
            {
                auto& state = a_peers[update.playerID];
                AppearanceSync::AcceptRemoteAppearance(state.appearance, update.playerID, std::move(update.appearance));
            }

            for (auto& update : changes.equipment)
            {
                auto& state = a_peers[update.playerID];
                EquipmentSync::AcceptRemoteEquipment(state.equipment, update.playerID, std::move(update.equipment));
            }

            for (auto& action : changes.animationActions)
            {
                const auto peer = a_peers.find(action.playerID);
                const bool known = peer != a_peers.end();
                CombatSync::AcceptRemoteAction(known ? &peer->second.combat : nullptr,
                    known ? peer->second.playout.GetNewest() : nullptr, std::move(action));
            }

            PlayerDetails::Accept(changes.inspections);
            TradeSync::QueueNetworkChanges(changes);
        }

        void PlayCheckpoints(Peers& a_peers)
        {
            PROFILE_FUNCTION();
            const auto now = CheckpointPlayout::Clock::now();
            for (auto& [playerID, peer] : a_peers)
            {
                peer.playout.Advance(playerID, now, [&peer](const CheckpointUpdate& a_update) {
                    FurnitureSync::OnCheckpointPlayed(peer.furniture, a_update);
                    AnimationSync::OnCheckpointPlayed(a_update);
                    CombatSync::OnCheckpointPlayed(peer.combat, a_update);
                });
            }
        }

        void YieldActivatedFurniture(Peers& a_peers, RE::PlayerCharacter& a_player)
        {
            PROFILE_FUNCTION();
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
                    ActorLifecycle::YieldFurniture(
                        GetRemoteFrame(playerID, peer), peer, activation.furnitureRefID, "local activation");
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

        void OpenActivatedPlayerMenu(const Peers& a_peers)
        {
            const auto actorID = s_localActorActivation.exchange(0, std::memory_order_acq_rel);
            auto* actor = actorID != 0 ? RE::TESForm::LookupByID<RE::Actor>(actorID) : nullptr;
            if (!actor)
            {
                return;
            }
            const auto handle = actor->GetHandle().native_handle();
            for (const auto& [playerID, peer] : a_peers)
            {
                const auto& lifecycle = peer.lifecycle;
                if ((lifecycle.actor && lifecycle.actor->handle.native_handle() == handle)
                    || (lifecycle.mount && lifecycle.mount->actor.handle.native_handle() == handle))
                {
                    PlayerActivation::Activate(playerID);
                    return;
                }
            }
        }

        void ServePlayerDetails(const Peers& a_peers)
        {
            const auto playerID = PlayerDetails::TakeRequest();
            if (!playerID)
            {
                return;
            }
            const auto peer = a_peers.find(*playerID);
            PlayerDetails::Serve(*playerID, peer != a_peers.end() ? &peer->second : nullptr);
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
            s_actorsExcludedFromSaves = false;
        }

        void SyncLocalPlayer(RE::PlayerCharacter& a_player, float a_delta)
        {
            PROFILE_FUNCTION();
            const LocalFrame local{ .player = a_player };

            const auto capturedAttack = CombatSync::CaptureLocalAttack(local);
            const bool handPresentationStarted = CombatSync::CaptureLocalHandPresentation(local);
            const auto graphEpoch = AnimationSync::ObserveLocalGraphEpoch(local);
            // Flush equipment before a captured attack or hand presentation is published, so
            // peers receive a weapon swap ahead of its use.
            EquipmentSync::SyncEquipment(local, a_delta, capturedAttack.has_value() || handPresentationStarted);
            CombatSync::QueueLocalAttacks(capturedAttack, graphEpoch.changed);
            const auto publication = CheckpointPublisher::PublishLocalCheckpoint(local, a_delta, graphEpoch.epoch);
            switch (publication.status)
            {
                case CheckpointPublisher::Status::kDisconnected:
                    CombatSync::DiscardLocalAttacks();
                    break;
                case CheckpointPublisher::Status::kPublished:
                    // The checkpoint always precedes the action it anchors.
                    CombatSync::PublishLocalAttacks(publication.checkpoint);
                    break;
                default:
                    break;
            }
            AppearanceSync::SyncAppearance(local, a_delta);
        }

        void SyncPeer(Multiplayer::PeerSlot a_playerID, PeerState& a_peer, const LocalFrame& a_local,
            RE::FormID a_localCellID, RE::FormID a_localWorldspaceID, RE::TESObjectREFR* a_localFurniture,
            float a_delta)
        {
            PROFILE_FUNCTION();
            const auto remote = GetRemoteFrame(a_playerID, a_peer);
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
                CombatSync::ClearRemoteActions(a_peer.combat);
                return;
            }
            if (lifecycle.actor && DeathSync::SyncDeath(remote, a_peer))
            {
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
                ActorLifecycle::RetireWhenStranded(remote, a_peer, a_delta);
            }
            if (lifecycle.yieldedFurnitureRefID != remote.furnitureRefID)
            {
                if (const auto* reason = FurnitureSync::FindFurnitureYieldReason(
                        a_local, remote, a_localFurniture, lifecycle.actor ? &*lifecycle.actor : nullptr))
                {
                    ActorLifecycle::YieldFurniture(remote, a_peer, remote.furnitureRefID, reason);
                }
            }

            if (lifecycle.actor && MountSync::SyncMount(remote, a_peer, a_delta))
            {
                auto& actor = *lifecycle.actor;
                CombatSync::ClearRemoteActions(a_peer.combat);
                EquipmentSync::SyncEquipment(a_peer.equipment, a_playerID, actor, a_delta);
                const bool readyForWeaponDraw = EquipmentSync::IsReadyForWeaponDraw(a_peer.equipment);
                if (!MountSync::UpdateRider(remote, a_peer, readyForWeaponDraw, a_delta))
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
                    AnimationSync::SyncAnimation(remote, actor, readyForWeaponDraw, a_delta);
                }
                furniturePlaying = FurnitureSync::SyncFurnitureEntry(remote, actor, attackPlaying, furniturePlaying);
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
            if (lifecycle.yieldedFurnitureRefID != 0 && lifecycle.yieldedFurnitureRefID == remote.furnitureRefID
                && FurnitureSync::IsVacated(a_local, lifecycle.yieldedFurnitureRefID))
            {
                ActorLifecycle::EndFurnitureYield(remote, a_peer);
            }
            ActorLifecycle::SpawnWhenDue(remote, a_peer, a_delta);
        }
    }

    void OnFrame(RE::PlayerCharacter* a_player, float a_delta)
    {
        PROFILE_FUNCTION();
        PROFILE_PLOT("Remote peers", s_peers.size());
        // A pass that returns before the peers are synced leaves no plates behind.
        NameplateSync::Clear();
        RestoreSaveEligibility(s_peers);
        DefaultOutfit::Refresh();
        ApplyNetworkChanges(s_peers);
        ServePlayerDetails(s_peers);
        PlayCheckpoints(s_peers);
        ActorLifecycle::RefillSpawnAllowance(a_delta);
        if (!a_player)
        {
            return;
        }

        YieldActivatedFurniture(s_peers, *a_player);
        OpenActivatedPlayerMenu(s_peers);
        const bool inWorld = PresenceSync::SyncPresence(LocalFrame{ .player = *a_player });
        if (inWorld)
        {
            SyncLocalPlayer(*a_player, a_delta);
        }
        else
        {
            // Attacks captured before leaving would otherwise anchor to the first checkpoint after returning.
            CombatSync::DiscardLocalAttacks();
        }
        TradeSync::Sync(*a_player, inWorld, a_delta);

        auto* localCell = a_player->GetParentCell();
        if (!localCell)
        {
            return;
        }
        PrecisionIsolation::Update(*localCell, a_delta);

        const LocalFrame local{ .player = *a_player };
        const auto localCellID = localCell->GetFormID();
        const auto localWorldspaceID = MovementSync::GetWorldspaceID(*a_player);
        const auto localFurniture = a_player->GetOccupiedFurniture().get();
        for (auto& [playerID, peer] : s_peers)
        {
            SyncPeer(playerID, peer, local, localCellID, localWorldspaceID, localFurniture.get(), a_delta);
        }
        NameplateSync::Update(s_peers, *a_player, a_delta);
    }

    void QueueLocalFurnitureActivation(RE::FormID a_furnitureRefID, bool a_deferred)
    {
        const std::scoped_lock lock{ s_localFurnitureActivationsLock };
        s_localFurnitureActivations.push_back({ .furnitureRefID = a_furnitureRefID, .deferred = a_deferred });
    }

    void QueueLocalActorActivation(RE::FormID a_actorID)
    {
        s_localActorActivation.store(a_actorID, std::memory_order_release);
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
        // Every peer is restored, so an actor spawned after this is unaffected.
        s_actorsExcludedFromSaves = true;
        logs::debug("Excluded {} remote player actors from game save", excluded);
    }
}
