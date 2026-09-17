#include "sync/combat/CombatSync.h"
#include "actor/ActorAnimation.h"
#include "actor/RemoteActor.h"
#include "network/SocketIOClient.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace CombatSync
{
	namespace
	{
		// An attack is presentation of what the peer did just now. Replaying it
		// seconds later only extends the swing sequence past the peer's own; this
		// still covers a weapon draw or swing that the attack waits behind.
		constexpr float kAnimationActionTimeoutSeconds = 3.0F;
		constexpr std::uint32_t kMaximumAnimationActionAttempts = 4;
		// Rejected attacks are also retried for at least this long, so a high frame
		// rate cannot spend every attempt inside one short graph blend.
		constexpr float kMinimumAnimationActionRetrySeconds = 0.25F;
		constexpr std::size_t kMaximumQueuedAnimationActions = 32;

		// Captured local attacks waiting for the next published checkpoint.
		std::uint32_t s_pendingNormalAttacks = 0;

		constexpr bool IsMeleeAttackState(RE::ATTACK_STATE_ENUM a_state)
		{
			switch (a_state) {
			case RE::ATTACK_STATE_ENUM::kDraw:
			case RE::ATTACK_STATE_ENUM::kSwing:
			case RE::ATTACK_STATE_ENUM::kHit:
			case RE::ATTACK_STATE_ENUM::kNextAttack:
			case RE::ATTACK_STATE_ENUM::kFollowThrough:
				return true;
			default:
				return false;
			}
		}
	}

	bool CaptureLocalAttack(const LocalFrame& a_local)
	{
		static RE::ATTACK_STATE_ENUM previousAttackState =
			RE::ATTACK_STATE_ENUM::kNone;
		static bool attackWindowActive = false;

		const auto* actorState = a_local.player.AsActorState();
		const auto attackState = actorState ? actorState->GetAttackState() :
			RE::ATTACK_STATE_ENUM::kNone;
		const bool meleeAttackActive = IsMeleeAttackState(attackState);
		const bool priorAttackFinishing =
			previousAttackState == RE::ATTACK_STATE_ENUM::kNextAttack ||
			previousAttackState == RE::ATTACK_STATE_ENUM::kFollowThrough;
		const bool nextSwingStarted = attackWindowActive &&
			priorAttackFinishing &&
			(attackState == RE::ATTACK_STATE_ENUM::kDraw ||
				attackState == RE::ATTACK_STATE_ENUM::kSwing);
		const bool attackStarted = meleeAttackActive &&
			(!attackWindowActive || nextSwingStarted);

		attackWindowActive = meleeAttackActive;
		previousAttackState = attackState;
		if (attackStarted) {
			logs::info(
				"Animation sync observed local melee attack state edge: state={} normalizedAction=attack/normal powerAttack={}",
				static_cast<std::underlying_type_t<RE::ATTACK_STATE_ENUM>>(
					attackState),
				a_local.player.IsPowerAttacking());
		}
		return attackStarted;
	}

	void QueueLocalAttacks(std::uint32_t a_capturedAttacks, bool a_graphEpochChanged)
	{
		s_pendingNormalAttacks = (std::min)(
			s_pendingNormalAttacks + a_capturedAttacks,
			static_cast<std::uint32_t>(kMaximumQueuedAnimationActions));
		if (a_graphEpochChanged) {
			// An accepted input from the previous graph cannot be attached to a
			// checkpoint from this new epoch. Historical attacks are not replayed.
			s_pendingNormalAttacks = 0;
		}
	}

	void DiscardLocalAttacks()
	{
		s_pendingNormalAttacks = 0;
	}

	void PublishLocalAttacks(const Network::PlayerTransform& a_checkpoint)
	{
		if (a_checkpoint.furnitureRefID != 0) {
			s_pendingNormalAttacks = 0;
		} else if (a_checkpoint.animationState.weaponDrawn) {
			while (s_pendingNormalAttacks != 0) {
				Network::PublishLocalNormalAttack(a_checkpoint.graphEpoch, a_checkpoint.sequence);
				--s_pendingNormalAttacks;
			}
		}
	}

	void OnCheckpointAccepted(RemoteState& a_state, const CheckpointUpdate& a_update)
	{
		if (!a_update.inserted && a_update.previous.graphEpoch !=
			a_update.checkpoint.graphEpoch) {
			const auto newEpoch = a_update.checkpoint.graphEpoch;
			const auto removed = std::erase_if(
				a_state.pendingAnimationActions,
				[newEpoch](const PendingAnimationAction& a_pending) {
					return a_pending.action.graphEpoch != newEpoch;
				});
			if (removed != 0) {
				logs::info(
					"Animation sync rejected queued actions at graph-epoch boundary: player={} graphEpoch={} rejected={}",
					a_update.playerID,
					static_cast<unsigned long long>(newEpoch),
					removed);
			}
		}
		if (a_update.checkpoint.furnitureRefID != 0 &&
			!a_state.pendingAnimationActions.empty()) {
			logs::info(
				"Animation sync discarded queued attacks at furniture checkpoint: player={} furniture={:08X} rejected={}",
				a_update.playerID,
				a_update.checkpoint.furnitureRefID,
				a_state.pendingAnimationActions.size());
			a_state.pendingAnimationActions.clear();
		}
	}

	void AcceptRemoteAction(
		RemoteState* a_state,
		const Network::PlayerTransform* a_checkpoint,
		Network::AnimationAction&& a_action)
	{
		if (!a_state || !a_checkpoint ||
			a_action.graphEpoch < a_checkpoint->graphEpoch ||
			a_checkpoint->furnitureRefID != 0) {
			logs::info(
				"Animation sync rejected action without a compatible checkpoint: player={} actionSequence={} graphEpoch={}",
				a_action.playerID,
				static_cast<unsigned long long>(a_action.actionSequence),
				static_cast<unsigned long long>(a_action.graphEpoch));
			return;
		}
		if (a_state->pendingAnimationActions.size() >=
			kMaximumQueuedAnimationActions) {
			logs::warn(
				"Animation sync rejected action because the bounded queue is full: player={} actionSequence={}",
				a_action.playerID,
				static_cast<unsigned long long>(a_action.actionSequence));
			return;
		}
		a_state->pendingAnimationActions.push_back(
			PendingAnimationAction{ .action = std::move(a_action) });
	}

	void AgeRemoteActions(const RemoteFrame& a_remote, RemoteState& a_state, float a_delta)
	{
		for (auto& pending : a_state.pendingAnimationActions) {
			pending.age += (std::max)(a_delta, 0.0F);
			if (pending.dispatchAttempts != 0) {
				pending.retryElapsed += (std::max)(a_delta, 0.0F);
			}
		}
		while (!a_state.pendingAnimationActions.empty() &&
			a_state.pendingAnimationActions.front().age >=
				kAnimationActionTimeoutSeconds) {
			logs::warn(
				"Animation sync timed out queued attack: player={} actionSequence={} graphEpoch={}",
				a_remote.playerID,
				static_cast<unsigned long long>(
					a_state.pendingAnimationActions.front().action.actionSequence),
				static_cast<unsigned long long>(
					a_state.pendingAnimationActions.front().action.graphEpoch));
			a_state.pendingAnimationActions.pop_front();
		}
	}

	void ClearRemoteActions(RemoteState& a_state)
	{
		a_state.pendingAnimationActions.clear();
	}

	void SyncCombat(
		const RemoteFrame& a_remote,
		RemoteState& a_state,
		RemoteActor& a_actor,
		bool a_handsSettled)
	{
		if (a_state.pendingAnimationActions.empty()) {
			return;
		}
		// A playing attack only takes the next one as a chained swing in its combo
		// window. Waiting for the whole recovery instead makes every replayed swing
		// slower than the local chain, so spammed attacks pile up behind it.
		const bool chaining = ActorAnimation::IsAnimationActionPlaying(a_actor);
		if (chaining && !ActorAnimation::CanChainNormalAttack(a_actor)) {
			return;
		}

		auto& pending = a_state.pendingAnimationActions.front();
		const auto& action = pending.action;
		if (action.graphEpoch != a_remote.checkpoint.graphEpoch ||
			action.checkpointSequence > a_remote.lastSequence ||
			a_remote.checkpoint.animationState.jumping ||
			a_remote.checkpoint.furnitureRefID != 0 ||
			!a_remote.checkpoint.animationState.weaponDrawn ||
			// A weapon swap published before the attack must reach the hands first.
			!a_handsSettled ||
			(!chaining && !ActorAnimation::CanDispatchNormalAttack(a_actor))) {
			return;
		}

		const bool accepted = ActorAnimation::DispatchNormalAttack(
			a_actor);
		if (!accepted && chaining) {
			// A closed combo window is not a rejection of the attack itself; it
			// starts normally once the playing attack completes.
			return;
		}
		++pending.dispatchAttempts;
		if (accepted) {
			logs::info(
				"Animation sync processed normal attack: player={} actor={:08X} actionSequence={} graphEpoch={} checkpointSequence={} chained={} queued={}",
				a_remote.playerID,
				a_actor.handle.native_handle(),
				static_cast<unsigned long long>(action.actionSequence),
				static_cast<unsigned long long>(action.graphEpoch),
				static_cast<unsigned long long>(action.checkpointSequence),
				chaining,
				a_state.pendingAnimationActions.size() - 1);
			a_state.pendingAnimationActions.pop_front();
		} else if (pending.dispatchAttempts >=
				kMaximumAnimationActionAttempts &&
			pending.retryElapsed >= kMinimumAnimationActionRetrySeconds) {
			logs::warn(
				"Animation sync rejected normal attack after bounded retries: player={} actor={:08X} actionSequence={} attempts={} retrySeconds={:.3f}",
				a_remote.playerID,
				a_actor.handle.native_handle(),
				static_cast<unsigned long long>(action.actionSequence),
				pending.dispatchAttempts,
				pending.retryElapsed);
			a_state.pendingAnimationActions.pop_front();
		}
	}
}
