#include "sync/SyncCoordinator.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRuntime.h"
#include "network/PacketDelay.h"
#include "network/SocketIOClient.h"
#include "runtime/GameThread.h"
#include "sync/ActorLifecycle.h"
#include "sync/CheckpointPublisher.h"
#include "sync/PeerState.h"
#include "sync/SyncFrame.h"
#include "sync/animation/AnimationSync.h"
#include "sync/appearance/AppearanceSync.h"
#include "sync/combat/CombatSync.h"
#include "sync/equipment/EquipmentSync.h"
#include "sync/furniture/FurnitureSync.h"
#include "sync/movement/MovementSync.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace SyncCoordinator
{
	namespace
	{
		// A brief interruption keeps actors so automatic reconnects are seamless;
		// a server silent this long (latency probes go out every second) means
		// the peers' state cannot be trusted.
		constexpr float kConnectionLossDespawnSeconds = 10.0F;

		// Accessed only on the bound main-loop thread.
		Peers s_peers;
		std::uint64_t s_observedServerResponses = 0;
		float s_serverSilenceElapsed = 0.0F;
		bool s_actorsExcludedFromSaves = false;

		// Receive stage. Transforms are latest-wins by sequence, appearance and
		// equipment by revision; actions are ordered and bounded; a disconnect
		// clears everything for that player.
		void ApplyNetworkChanges(Peers& a_peers)
		{
			auto changes = Network::DrainRemotePlayerChanges();

			for (const auto& playerID : changes.disconnectedPlayerIDs) {
				const auto peer = a_peers.find(playerID);
				if (peer == a_peers.end()) {
					continue;
				}

				ActorLifecycle::RemoveOnDisconnect(peer->second);
				a_peers.erase(peer);
				logs::info(
					"Removed remote player actor {} after server player:left",
					playerID);
			}

			for (auto& update : changes.transforms) {
				auto [peer, inserted] = a_peers.try_emplace(update.playerID);
				auto& state = peer->second;
				if (!inserted && update.transform.sequence <= state.lastSequence) {
					logs::info(
						"Animation sync ignored stale main-loop snapshot: player={} sequence={} state={} newestSequence={} newestState={}",
						update.playerID,
						static_cast<unsigned long long>(update.transform.sequence),
						Multiplayer::GetMovementStateName(
							update.transform.animationState.movementState),
						static_cast<unsigned long long>(state.lastSequence),
						Multiplayer::GetMovementStateName(
							state.latestCheckpoint.animationState.movementState));
					continue;
				}

				const CheckpointUpdate checkpointUpdate{
					.playerID = update.playerID,
					.inserted = inserted,
					.previous = state.latestCheckpoint,
					.previousSequence = state.lastSequence,
					.checkpoint = update.transform
				};
				if (update.sentAtServerMilliseconds) {
					Network::RecordRemotePacketDelay(*update.sentAtServerMilliseconds);
				}
				FurnitureSync::OnCheckpointAccepted(checkpointUpdate);
				AnimationSync::OnCheckpointAccepted(checkpointUpdate);
				CombatSync::OnCheckpointAccepted(state.combat, checkpointUpdate);
				state.latestCheckpoint = update.transform;
				state.lastSequence = update.transform.sequence;
			}

			for (auto& update : changes.appearances) {
				// An appearance may arrive before the first checkpoint. The actor is
				// only spawned once a checkpoint places it in the local space.
				auto& state = a_peers[update.playerID];
				AppearanceSync::AcceptRemoteAppearance(
					state.appearance,
					update.playerID,
					std::move(update.appearance));
			}

			for (auto& update : changes.equipment) {
				// Equipment is kept independent of whether an actor reference exists,
				// so every respawn reconciles from the newest revision.
				auto& state = a_peers[update.playerID];
				EquipmentSync::AcceptRemoteEquipment(
					state.equipment,
					update.playerID,
					std::move(update.equipment));
			}

			for (auto& action : changes.animationActions) {
				const auto peer = a_peers.find(action.playerID);
				const bool known = peer != a_peers.end();
				CombatSync::AcceptRemoteAction(
					known ? &peer->second.combat : nullptr,
					known ? &peer->second.latestCheckpoint : nullptr,
					std::move(action));
			}
		}

		void RestoreSaveEligibility(Peers& a_peers)
		{
			if (!s_actorsExcludedFromSaves) {
				return;
			}
			auto* saveLoadGame = RE::BGSSaveLoadGame::GetSingleton();
			if (saveLoadGame && saveLoadGame->GetSaveGameSaving()) {
				return;
			}

			// The flag is only meant for the save writer; its other engine uses
			// concern references pending destruction, so it is not kept longer.
			for (auto& [playerID, peer] : a_peers) {
				ActorLifecycle::SetExcludedFromSaves(peer, false);
			}
			s_actorsExcludedFromSaves = false;
		}

		void DespawnAfterConnectionLoss(Peers& a_peers, float a_delta)
		{
			// Socket.IO may keep a dead transport marked connected, so silence is
			// measured by server acknowledgements rather than by IsConnected.
			const auto responses = Network::GetServerResponseCount();
			if (responses != s_observedServerResponses) {
				s_observedServerResponses = responses;
				s_serverSilenceElapsed = 0.0F;
				return;
			}
			if (s_serverSilenceElapsed >= kConnectionLossDespawnSeconds) {
				// Already despawned during this outage.
				return;
			}

			s_serverSilenceElapsed += std::clamp(
				a_delta,
				0.0F,
				ActorConstants::kMaximumFrameTime);
			if (s_serverSilenceElapsed < kConnectionLossDespawnSeconds ||
				a_peers.empty()) {
				return;
			}

			for (auto& [playerID, peer] : a_peers) {
				ActorLifecycle::RemoveOnDisconnect(peer);
			}
			logs::info(
				"Removed {} remote player actors after {:.0f} seconds without a server response: connected={}",
				a_peers.size(),
				kConnectionLossDespawnSeconds,
				Network::IsConnected());
			a_peers.clear();
			// Reconnecting rebuilds every peer from the server's resent state.
			Network::ForgetRemotePlayers();
			if (auto* console = RE::ConsoleLog::GetSingleton()) {
				console->Print("MP: Server connection lost; removed remote players.");
			}
		}

		// Local stages. Attack capture and graph-epoch observation run every pass;
		// the checkpoint and its attacks are published at the snapshot cadence.
		void SyncLocalPlayer(RE::PlayerCharacter& a_player, float a_delta)
		{
			const LocalFrame local{ .player = a_player };

			const bool attackCaptured = CombatSync::CaptureLocalAttack(local);
			const bool handPresentationStarted =
				CombatSync::CaptureLocalHandPresentation(local);
			const auto graphEpoch = AnimationSync::ObserveLocalGraphEpoch(local);
			// Flush equipment before a captured attack, bow draw, or cast can be
			// published, so peers receive a weapon swap ahead of its use.
			EquipmentSync::SyncEquipment(
				local,
				a_delta,
				attackCaptured || handPresentationStarted);
			CombatSync::QueueLocalAttacks(attackCaptured ? 1 : 0, graphEpoch.changed);
			const auto publication = CheckpointPublisher::PublishLocalCheckpoint(
				local,
				a_delta,
				graphEpoch.epoch);
			switch (publication.status) {
			case CheckpointPublisher::Status::kDisconnected:
				CombatSync::DiscardLocalAttacks();
				break;
			case CheckpointPublisher::Status::kPublished:
				// player:update always precedes the animation:action it anchors.
				CombatSync::PublishLocalAttacks(publication.checkpoint);
				break;
			default:
				break;
			}
			AppearanceSync::SyncAppearance(local, a_delta);
		}

		// Remote stages for one peer.
		void SyncPeer(
			const std::string& a_playerID,
			PeerState& a_peer,
			RE::TESNPC* a_localPlayerBase,
			RE::FormID a_localCellID,
			RE::FormID a_localWorldspaceID,
			float a_delta)
		{
			const RemoteFrame remote{
				.playerID = a_playerID,
				.checkpoint = a_peer.latestCheckpoint,
				.lastSequence = a_peer.lastSequence
			};
			auto& lifecycle = a_peer.lifecycle;

			const bool isInLocalSpace = MovementSync::IsInLocalSpace(
				remote,
				a_localCellID,
				a_localWorldspaceID);
			const bool changedSpawnedSpace =
				ActorLifecycle::HasChangedSpawnedSpace(a_peer);
			AppearanceSync::SyncAppearance(a_peer.appearance, a_delta);
			if (lifecycle.actor && isInLocalSpace && !changedSpawnedSpace &&
				AppearanceSync::RequiresRespawn(a_peer.appearance)) {
				ActorLifecycle::RetireForAppearanceChange(remote, a_peer);
			}
			if (lifecycle.actor && (!isInLocalSpace || changedSpawnedSpace)) {
				ActorLifecycle::RetireForSpaceChange(
					remote,
					a_peer,
					a_localCellID,
					a_localWorldspaceID);
			}

			CombatSync::AgeRemoteActions(remote, a_peer.combat, a_delta);
			if (!isInLocalSpace) {
				// Do not preserve transient combat history while no actor/checkpoint
				// can be reconstructed in the local coordinate space.
				CombatSync::ClearRemoteActions(a_peer.combat);
				return;
			}
			if (lifecycle.actor) {
				// Same-space fast travel keeps the coordinate space, so it is
				// detected by displacement instead. The actor respawns below.
				ActorLifecycle::RetireWhenStranded(remote, a_peer, a_delta);
			}

			if (lifecycle.actor) {
				auto& actor = *lifecycle.actor;
				MovementSync::SyncMovement(remote, a_peer.appliedSequence, actor);
				EquipmentSync::SyncEquipment(
					a_peer.equipment,
					a_playerID,
					actor,
					a_delta);
				const bool handsSettled =
					EquipmentSync::AreHandsSettled(a_peer.equipment);
				const bool readyForWeaponDraw =
					EquipmentSync::IsReadyForWeaponDraw(a_peer.equipment);

				bool furniturePlaying = FurnitureSync::SyncFurniture(remote, actor);
				const bool attackPlaying =
					ActorAnimation::IsAnimationActionPlaying(actor);
				if (!attackPlaying && !furniturePlaying) {
					AnimationSync::SyncAnimation(remote, actor, readyForWeaponDraw);
				}
				furniturePlaying = FurnitureSync::SyncFurnitureEntry(
					remote,
					actor,
					attackPlaying,
					furniturePlaying);
				// Stage the freshest base state before considering an action. If this
				// changes the graph request, CanDispatchNormalAttack keeps the action
				// queued until ApplyAnimationUpdate settles it on a following frame.
				if (!furniturePlaying) {
					CombatSync::SyncCombat(remote, a_peer.combat, actor, handsSettled);
					CombatSync::SyncHandPresentation(remote, actor, handsSettled, a_delta);
				}
				const auto translationState = AnimationSync::GetTranslationState(
					remote,
					actor,
					readyForWeaponDraw,
					furniturePlaying);

				if (!ActorRuntime::Update(actor, a_delta, translationState)) {
					ActorLifecycle::OnActorLost(remote, a_peer);
				} else if (auto liveActor = actor.handle.get()) {
					AppearanceSync::ApplyActorPresentation(
						a_peer.appearance,
						a_playerID,
						*liveActor,
						actor.ready);
				}
			}

			if (lifecycle.actor) {
				return;
			}
			ActorLifecycle::SpawnWhenDue(remote, a_peer, a_localPlayerBase, a_delta);
		}
	}

	void OnFrame(RE::PlayerCharacter* a_player, float a_delta)
	{
		RestoreSaveEligibility(s_peers);
		ApplyNetworkChanges(s_peers);
		DespawnAfterConnectionLoss(s_peers, a_delta);
		if (!a_player) {
			return;
		}

		SyncLocalPlayer(*a_player, a_delta);

		auto* localCell = a_player->GetParentCell();
		auto* playerBase = a_player->GetActorBase();
		if (!localCell || !playerBase) {
			return;
		}

		for (auto& [playerID, peer] : s_peers) {
			SyncPeer(
				playerID,
				peer,
				playerBase,
				localCell->GetFormID(),
				MovementSync::GetWorldspaceID(*a_player),
				a_delta);
		}
	}

	void OnGameSave()
	{
		if (!GameThread::Require("ExcludeRemoteActorsFromSave")) {
			// Touching peers here would race the main loop; the save keeps actors.
			return;
		}

		std::size_t excluded = 0;
		for (auto& [playerID, peer] : s_peers) {
			if (peer.lifecycle.actor) {
				ActorLifecycle::SetExcludedFromSaves(peer, true);
				++excluded;
			}
		}
		// Every peer is restored, so an actor spawned after this is unaffected.
		s_actorsExcludedFromSaves = true;
		logs::info("Excluded {} remote player actors from game save", excluded);
	}
}
