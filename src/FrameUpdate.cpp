#include "FrameUpdate.h"
#include "NPCSpawner.h"
#include "PlayerAppearance.h"
#include "PlayerEquipment.h"
#include "SocketIOClient.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>

namespace FrameUpdate
{
	namespace
	{
		constexpr float kPuppetSpeed = 550.0F;
		constexpr float kPuppetStoppingDistance = 1.0F;
		constexpr float kSpawnRetryDelay = 1.0F;
		constexpr float kSnapshotInterval = 0.05F;
		constexpr float kMinimumHeadingTravel = 1.0F;
		constexpr float kTwoPi = 6.28318530717958647692F;
		constexpr float kAnimationActionTimeout = 10.0F;
		constexpr std::uint32_t kMaximumAnimationActionAttempts = 4;
		// Rejected attacks are also retried for at least this long, so a high frame
		// rate cannot spend every attempt inside one short graph blend.
		constexpr float kMinimumAnimationActionRetrySeconds = 0.25F;
		constexpr std::size_t kMaximumQueuedAnimationActions = 32;
		struct PendingAnimationAction
		{
			Network::AnimationAction action;
			float age{ 0.0F };
			std::uint32_t dispatchAttempts{ 0 };
			// Time since the first rejected dispatch attempt.
			float retryElapsed{ 0.0F };
		};
		struct RemotePuppet
		{
			Network::PlayerTransform target;
			std::uint64_t lastSequence{ 0 };
			std::uint64_t appliedSequence{ 0 };
			std::deque<PendingAnimationAction> pendingAnimationActions;
			std::optional<NPCSpawner::ControlledNPC> controlledNPC;
			PlayerAppearance::RemoteAppearance appearance;
			PlayerEquipment::RemoteEquipment equipment;
			RE::FormID spawnedCellID{ 0 };
			RE::FormID spawnedWorldspaceID{ 0 };
			float spawnRetryRemaining{ 0.0F };
		};

		using RemotePuppets = std::unordered_map<std::string, RemotePuppet>;

		constexpr bool IsMoving(Multiplayer::MovementState a_state)
		{
			return a_state != Multiplayer::MovementState::kIdle &&
				a_state != Multiplayer::MovementState::kSneakingIdle;
		}

		constexpr bool IsSneaking(Multiplayer::MovementState a_state)
		{
			return a_state == Multiplayer::MovementState::kSneakingIdle ||
				a_state == Multiplayer::MovementState::kSneakingWalking ||
				a_state == Multiplayer::MovementState::kSneakingRunning;
		}

		constexpr Multiplayer::MovementState GetNormalMovingState(
			Multiplayer::MovementState a_state)
		{
			return a_state == Multiplayer::MovementState::kWalking ||
				a_state == Multiplayer::MovementState::kSneakingWalking ?
				Multiplayer::MovementState::kWalking :
				Multiplayer::MovementState::kRunning;
		}

		constexpr Multiplayer::MovementState GetNextMovementStep(
			Multiplayer::MovementState a_current,
			Multiplayer::MovementState a_target)
		{
			if (a_current == a_target) {
				return a_target;
			}

			const bool currentSprinting =
				a_current == Multiplayer::MovementState::kSprinting;
			const bool targetSprinting =
				a_target == Multiplayer::MovementState::kSprinting;
			if (currentSprinting && !targetSprinting) {
				// Leave sprint while still moving. A later graph update can stop or
				// enter sneak without racing SprintStop against another event.
				return IsMoving(a_target) && !IsSneaking(a_target) ?
					GetNormalMovingState(a_target) :
					Multiplayer::MovementState::kRunning;
			}
			if (!currentSprinting && targetSprinting) {
				if (IsSneaking(a_current)) {
					return IsMoving(a_current) ?
						GetNormalMovingState(a_current) :
						Multiplayer::MovementState::kIdle;
				}
				if (!IsMoving(a_current)) {
					return Multiplayer::MovementState::kRunning;
				}
				return a_target;
			}

			const bool currentSneaking = IsSneaking(a_current);
			const bool targetSneaking = IsSneaking(a_target);
			if (currentSneaking != targetSneaking &&
				IsMoving(a_current) != IsMoving(a_target)) {
				// When both sneak and movement change, change only one graph axis at
				// a time. Preserve the current sneak axis while stopping, or establish
				// the target sneak axis while still idle before starting movement.
				if (IsMoving(a_current)) {
					return currentSneaking ?
						Multiplayer::MovementState::kSneakingIdle :
						Multiplayer::MovementState::kIdle;
				}
				return targetSneaking ?
					Multiplayer::MovementState::kSneakingIdle :
					Multiplayer::MovementState::kIdle;
			}

			return a_target;
		}

		constexpr Multiplayer::AnimationState GetNextAnimationStep(
			Multiplayer::AnimationState a_current,
			Multiplayer::AnimationState a_target)
		{
			if (a_current == a_target) {
				return a_target;
			}
			if (a_current.jumping && a_target.jumping) {
				// Locomotion events are rejected by the graph's airborne branch. Keep
				// the takeoff locomotion underneath the jump and apply the freshest
				// movement state when the latest checkpoint becomes grounded.
				return a_current;
			}

			// Takeoff is entered only after its grounded locomotion prerequisites
			// settle. Landing adopts the newest checkpoint's locomotion variables;
			// its visible locomotion events are reasserted after the landing branch.
			if (a_current.jumping != a_target.jumping) {
				if (a_current.jumping) {
					return a_target;
				}

				auto groundedTarget = a_target;
				groundedTarget.jumping = false;
				if (a_current != groundedTarget) {
					return GetNextAnimationStep(a_current, groundedTarget);
				}
				return a_target;
			}

			return Multiplayer::AnimationState{
				.movementState = GetNextMovementStep(
					a_current.movementState,
					a_target.movementState),
				.jumping = a_target.jumping,
				.weaponDrawn = a_target.weaponDrawn
			};
		}

		static_assert(GetNextMovementStep(
			Multiplayer::MovementState::kIdle,
			Multiplayer::MovementState::kSprinting) ==
			Multiplayer::MovementState::kRunning);
		static_assert(GetNextMovementStep(
			Multiplayer::MovementState::kSprinting,
			Multiplayer::MovementState::kIdle) ==
			Multiplayer::MovementState::kRunning);
		static_assert(GetNextMovementStep(
			Multiplayer::MovementState::kIdle,
			Multiplayer::MovementState::kSneakingRunning) ==
			Multiplayer::MovementState::kSneakingIdle);
		static_assert(GetNextMovementStep(
			Multiplayer::MovementState::kSneakingRunning,
			Multiplayer::MovementState::kIdle) ==
			Multiplayer::MovementState::kSneakingIdle);
		static_assert(GetNextAnimationStep(
			Multiplayer::AnimationState{},
			Multiplayer::AnimationState{
				.movementState = Multiplayer::MovementState::kRunning,
				.jumping = true }) ==
			Multiplayer::AnimationState{
				.movementState = Multiplayer::MovementState::kRunning,
				.jumping = false });
		static_assert(GetNextAnimationStep(
			Multiplayer::AnimationState{
				.movementState = Multiplayer::MovementState::kRunning,
				.jumping = true },
			Multiplayer::AnimationState{
				.movementState = Multiplayer::MovementState::kIdle,
				.jumping = true }) ==
			Multiplayer::AnimationState{
				.movementState = Multiplayer::MovementState::kRunning,
				.jumping = true });
		static_assert(GetNextAnimationStep(
			Multiplayer::AnimationState{
				.movementState = Multiplayer::MovementState::kRunning,
				.jumping = true },
			Multiplayer::AnimationState{}) ==
			Multiplayer::AnimationState{});
		static_assert(GetNextAnimationStep(
			Multiplayer::AnimationState{},
			Multiplayer::AnimationState{ .weaponDrawn = true }) ==
			Multiplayer::AnimationState{ .weaponDrawn = true });

		Multiplayer::AnimationState GetAnimationState(RE::Actor& a_player)
		{
			// The graph enters bInJumpState early enough to produce a responsive
			// takeoff, but it can remain set throughout graph-owned landing cleanup.
			// Publish the start from the graph and the end from physical ground
			// contact so the remote puppet does not replay that cleanup as extra air
			// time before it is even allowed to land.
			static bool replicatedJumping = false;
			static bool observedMidair = false;
			static bool waitForGraphJumpReset = false;

			const auto* state = a_player.AsActorState();
			if (!state) {
				return {};
			}

			const bool isMoving = state->actorState1.movingForward ||
				state->actorState1.movingBack ||
				state->actorState1.movingLeft ||
				state->actorState1.movingRight;
			Multiplayer::MovementState movementState;
			if (state->IsSneaking()) {
				if (!isMoving) {
					movementState = Multiplayer::MovementState::kSneakingIdle;
				} else {
					movementState = state->IsRunning() ?
						Multiplayer::MovementState::kSneakingRunning :
						Multiplayer::MovementState::kSneakingWalking;
				}
			} else if (!isMoving) {
				movementState = Multiplayer::MovementState::kIdle;
			} else if (state->IsSprinting()) {
				movementState = Multiplayer::MovementState::kSprinting;
			} else if (state->IsRunning()) {
				movementState = Multiplayer::MovementState::kRunning;
			} else {
				movementState = Multiplayer::MovementState::kWalking;
			}

			static const RE::BSFixedString inJumpStateVariable{ "bInJumpState" };
			bool graphJumping = false;
			const bool graphJumpStateAvailable = a_player.GetGraphVariableBool(
				inJumpStateVariable,
				graphJumping);
			const bool physicalJumping = a_player.IsInJumpState();
			const bool physicalMidair = a_player.IsInMidair();
			const bool requestedJumping = graphJumpStateAvailable ?
				graphJumping : (physicalJumping || physicalMidair);

			if (replicatedJumping) {
				observedMidair = observedMidair || physicalMidair;
				if (observedMidair && !physicalMidair) {
					// Physical contact is the authoritative landing boundary. If the
					// graph is still in its landing branch, suppress that stale true
					// value until it has reset once.
					replicatedJumping = false;
					observedMidair = false;
					waitForGraphJumpReset = requestedJumping;
				} else if (!observedMidair && !requestedJumping) {
					// Also release a takeoff that was cancelled before leaving the ground.
					replicatedJumping = false;
				}
			} else if (waitForGraphJumpReset) {
				if (!requestedJumping) {
					waitForGraphJumpReset = false;
				} else if (physicalMidair) {
					// Skyrim accepts a new takeoff from its landing branch, so
					// bInJumpState may never reset between consecutive jumps. Leaving
					// the ground again after the published landing is that new jump.
					waitForGraphJumpReset = false;
					replicatedJumping = true;
					observedMidair = true;
				}
			} else if (requestedJumping) {
				replicatedJumping = true;
				observedMidair = physicalMidair;
			}

			return Multiplayer::AnimationState{
				.movementState = movementState,
				.jumping = replicatedJumping,
				.weaponDrawn = state->IsWeaponDrawn()
			};
		}

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

		bool ObserveLocalNormalAttack(RE::Actor& a_player)
		{
			static RE::ATTACK_STATE_ENUM previousAttackState =
				RE::ATTACK_STATE_ENUM::kNone;
			static bool attackWindowActive = false;

			const auto* actorState = a_player.AsActorState();
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
					a_player.IsPowerAttacking());
			}
			return attackStarted;
		}

		std::uint32_t GetWorldspaceID(const RE::TESObjectREFR& a_reference)
		{
			const auto* worldspace = a_reference.GetWorldspace();
			return worldspace ? worldspace->GetFormID() : 0;
		}

		std::uint32_t GetOccupiedFurnitureRefID(RE::Actor& a_actor)
		{
			auto furniture = a_actor.GetOccupiedFurniture().get();
			return furniture ? furniture->GetFormID() : 0;
		}

		constexpr bool IsSameCoordinateSpace(
			RE::FormID a_firstCellID,
			RE::FormID a_firstWorldspaceID,
			RE::FormID a_secondCellID,
			RE::FormID a_secondWorldspaceID)
		{
			// Exterior cells share world coordinates. They can synchronize across
			// their cell boundaries as long as both players are in the same world.
			if (a_firstWorldspaceID != 0 || a_secondWorldspaceID != 0) {
				return a_firstWorldspaceID != 0 &&
					a_firstWorldspaceID == a_secondWorldspaceID;
			}

			// Interior coordinates are local to their cell, so they only describe
			// the same space when the cell IDs match exactly.
			return a_firstCellID == a_secondCellID;
		}

		static_assert(IsSameCoordinateSpace(1, 0, 1, 0));
		static_assert(!IsSameCoordinateSpace(1, 0, 2, 0));
		static_assert(IsSameCoordinateSpace(1, 3, 2, 3));
		static_assert(!IsSameCoordinateSpace(1, 3, 1, 4));
		static_assert(!IsSameCoordinateSpace(1, 0, 1, 3));

		bool CanSynchronizeInLocalSpace(
			const Network::PlayerTransform& a_remote,
			RE::FormID a_localCellID,
			RE::FormID a_localWorldspaceID)
		{
			return IsSameCoordinateSpace(
				a_remote.cellID,
				a_remote.worldspaceID,
				a_localCellID,
				a_localWorldspaceID);
		}

		std::uint64_t ObserveLocalGraphEpoch(
			RE::Actor& a_player,
			bool& a_epochChanged)
		{
			static RE::BSAnimationGraphManager* observedGraph = nullptr;
			static std::uint64_t graphEpoch = 1;
			static bool graphWasObserved = false;
			static bool continuityLost = false;
			a_epochChanged = false;

			RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
			const bool graphAvailable = a_player.GetAnimationGraphManager(graphManager) &&
				graphManager;
			if (!graphAvailable) {
				continuityLost = continuityLost || graphWasObserved;
				observedGraph = nullptr;
				return graphEpoch;
			}

			if (!graphWasObserved) {
				graphWasObserved = true;
				observedGraph = graphManager.get();
				return graphEpoch;
			}
			if (continuityLost || observedGraph != graphManager.get()) {
				if (graphEpoch >= static_cast<std::uint64_t>(
						(std::numeric_limits<std::int64_t>::max)())) {
					logs::critical(
						"Animation sync cannot advance local graph epoch beyond the protocol limit");
					observedGraph = graphManager.get();
					continuityLost = false;
					return graphEpoch;
				}
				++graphEpoch;
				a_epochChanged = true;
				observedGraph = graphManager.get();
				continuityLost = false;
				logs::info(
					"Animation sync advanced local graph epoch: graphEpoch={}",
					static_cast<unsigned long long>(graphEpoch));
			}
			return graphEpoch;
		}

		void PublishLocalTransform(
			RE::Actor& a_player,
			float a_delta,
			std::uint64_t a_graphEpoch,
			bool a_graphEpochChanged,
			std::uint32_t a_capturedNormalAttacks)
		{
			static float elapsed = kSnapshotInterval;
			static std::uint64_t sequence = 0;
			static RE::NiPoint3 previousPosition{};
			static RE::FormID previousCellID = 0;
			static RE::FormID previousWorldspaceID = 0;
			static bool hasPreviousPosition = false;
			static Multiplayer::AnimationState previousAnimationState{};
			static bool hasPreviousAnimationState = false;
			static RE::FormID previousFurnitureRefID = 0;
			static bool hasPreviousFurnitureRefID = false;
			static std::uint32_t pendingNormalAttacks = 0;

			pendingNormalAttacks = (std::min)(
				pendingNormalAttacks + a_capturedNormalAttacks,
				static_cast<std::uint32_t>(kMaximumQueuedAnimationActions));
			if (a_graphEpochChanged) {
				// An accepted input from the previous graph cannot be attached to a
				// checkpoint from this new epoch. Historical attacks are not replayed.
				pendingNormalAttacks = 0;
			}

			elapsed += (std::max)(a_delta, 0.0F);
			if (elapsed < kSnapshotInterval || !Network::IsConnected()) {
				if (!Network::IsConnected()) {
					// Actions captured outside a connected stream have no valid sequence
					// boundary and must never be replayed after a later connection.
					pendingNormalAttacks = 0;
				}
				return;
			}
			const float snapshotElapsed = elapsed;
			elapsed = 0.0F;

			auto* cell = a_player.GetParentCell();
			if (!cell) {
				return;
			}

			if (sequence >= static_cast<std::uint64_t>(
					(std::numeric_limits<std::int64_t>::max)())) {
				sequence = 0;
			}
			++sequence;

			const auto position = a_player.GetPosition();
			const auto cellID = cell->GetFormID();
			const auto worldspaceID = GetWorldspaceID(a_player);
			const bool comparableLocalSpace = hasPreviousPosition &&
				((worldspaceID != 0 || previousWorldspaceID != 0) ?
					(worldspaceID != 0 && worldspaceID == previousWorldspaceID) :
					(cellID == previousCellID));

			float speed = 0.0F;
			// The body angle is the facing direction. Travel direction is carried
			// separately so a backpedalling puppet keeps facing forward.
			float heading = a_player.GetAngleZ();
			if (!std::isfinite(heading)) {
				heading = 0.0F;
			}
			static const RE::BSFixedString directionVariable{ "Direction" };
			float direction = 0.0F;
			const bool graphDirectionAvailable = a_player.GetGraphVariableFloat(
				directionVariable,
				direction) &&
				std::isfinite(direction);
			if (comparableLocalSpace && snapshotElapsed > 0.0F) {
				const float deltaX = position.x - previousPosition.x;
				const float deltaY = position.y - previousPosition.y;
				const float deltaZ = position.z - previousPosition.z;
				const float horizontalDistanceSquared =
					deltaX * deltaX + deltaY * deltaY;
				speed = std::sqrt(
					horizontalDistanceSquared + deltaZ * deltaZ) /
					snapshotElapsed;
				if (!std::isfinite(speed)) {
					speed = 0.0F;
				}

				if (!graphDirectionAvailable &&
					horizontalDistanceSquared >=
						kMinimumHeadingTravel * kMinimumHeadingTravel) {
					// A custom graph may not expose Direction. Skyrim heading zero faces
					// positive Y, so derive the travel angle relative to the body.
					direction = (std::atan2(deltaX, deltaY) - heading) / kTwoPi;
				}
			}
			direction = Multiplayer::NormalizeMovementDirection(direction);

			previousPosition = position;
			previousCellID = cellID;
			previousWorldspaceID = worldspaceID;
			hasPreviousPosition = true;
			const auto animationState = GetAnimationState(a_player);
			const auto furnitureRefID = GetOccupiedFurnitureRefID(a_player);
			if (!hasPreviousAnimationState ||
				animationState != previousAnimationState) {
				logs::info(
					"Animation sync publishing local state: sequence={} previous={}/{} state={}/{} position=({:.3f},{:.3f},{:.3f}) speed={:.1f} direction={:.3f} physicalJump={} physicalMidair={}",
					static_cast<unsigned long long>(sequence),
					hasPreviousAnimationState ?
						Multiplayer::GetMovementStateName(
							previousAnimationState.movementState) :
						"uninitialized",
					hasPreviousAnimationState && previousAnimationState.jumping ?
						"jumping" : "grounded",
					Multiplayer::GetMovementStateName(animationState.movementState),
					animationState.jumping ? "jumping" : "grounded",
					position.x,
					position.y,
					position.z,
					speed,
					direction,
					a_player.IsInJumpState(),
					a_player.IsInMidair());
				previousAnimationState = animationState;
				hasPreviousAnimationState = true;
			}
			if (!hasPreviousFurnitureRefID ||
				furnitureRefID != previousFurnitureRefID) {
				logs::info(
					"Animation sync publishing local furniture state: sequence={} previous={:08X} furniture={:08X}",
					static_cast<unsigned long long>(sequence),
					previousFurnitureRefID,
					furnitureRefID);
				previousFurnitureRefID = furnitureRefID;
				hasPreviousFurnitureRefID = true;
			}

			Network::PublishLocalPlayerTransform(Network::PlayerTransform{
				.x = position.x,
				.y = position.y,
				.z = position.z,
				.heading = heading,
				.speed = speed,
				.direction = direction,
				.animationState = animationState,
				.cellID = cellID,
				.worldspaceID = worldspaceID,
				.furnitureRefID = furnitureRefID,
				.graphEpoch = a_graphEpoch,
				.sequence = sequence
			});
			if (furnitureRefID != 0) {
				pendingNormalAttacks = 0;
			} else if (animationState.weaponDrawn) {
				while (pendingNormalAttacks != 0) {
					Network::PublishLocalNormalAttack(a_graphEpoch, sequence);
					--pendingNormalAttacks;
				}
			}
		}

		void ApplyNetworkChanges(RemotePuppets& a_puppets)
		{
			auto changes = Network::DrainRemotePlayerChanges();

			for (const auto& playerID : changes.disconnectedPlayerIDs) {
				const auto puppet = a_puppets.find(playerID);
				if (puppet == a_puppets.end()) {
					continue;
				}

				if (puppet->second.controlledNPC) {
					NPCSpawner::RemoveControlledNPC(*puppet->second.controlledNPC);
				}
				a_puppets.erase(puppet);
				logs::info(
					"Removed remote player puppet {} after server player:left",
					playerID);
			}

			for (auto& update : changes.transforms) {
				auto [puppet, inserted] = a_puppets.try_emplace(update.playerID);
				if (!inserted && update.transform.sequence <= puppet->second.lastSequence) {
					logs::info(
						"Animation sync ignored stale main-loop snapshot: player={} sequence={} state={} newestSequence={} newestState={}",
						update.playerID,
						static_cast<unsigned long long>(update.transform.sequence),
						Multiplayer::GetMovementStateName(
							update.transform.animationState.movementState),
						static_cast<unsigned long long>(puppet->second.lastSequence),
						Multiplayer::GetMovementStateName(
							puppet->second.target.animationState.movementState));
					continue;
				}

				const bool animationStateChanged = inserted ||
					puppet->second.target.animationState !=
						update.transform.animationState;
				if (puppet->second.target.furnitureRefID !=
					update.transform.furnitureRefID) {
					logs::info(
						"Animation sync accepted main-loop furniture target: player={} sequence={} previous={:08X} furniture={:08X}",
						update.playerID,
						static_cast<unsigned long long>(update.transform.sequence),
						puppet->second.target.furnitureRefID,
						update.transform.furnitureRefID);
				}
				if (animationStateChanged) {
					logs::info(
						"Animation sync accepted main-loop target: player={} previousSequence={} previous={}/{} sequence={} state={}/{} position=({:.3f},{:.3f},{:.3f}) speed={:.1f} direction={:.3f}",
						update.playerID,
						static_cast<unsigned long long>(puppet->second.lastSequence),
						inserted ?
							"uninitialized" :
							Multiplayer::GetMovementStateName(
								puppet->second.target.animationState.movementState),
						inserted || !puppet->second.target.animationState.jumping ?
							"grounded" : "jumping",
						static_cast<unsigned long long>(update.transform.sequence),
						Multiplayer::GetMovementStateName(
							update.transform.animationState.movementState),
						update.transform.animationState.jumping ?
							"jumping" : "grounded",
						update.transform.x,
						update.transform.y,
						update.transform.z,
						update.transform.speed,
						update.transform.direction);
				}
				if (!inserted && puppet->second.target.graphEpoch !=
					update.transform.graphEpoch) {
					const auto newEpoch = update.transform.graphEpoch;
					const auto removed = std::erase_if(
						puppet->second.pendingAnimationActions,
						[newEpoch](const PendingAnimationAction& a_pending) {
							return a_pending.action.graphEpoch != newEpoch;
						});
					if (removed != 0) {
						logs::info(
							"Animation sync rejected queued actions at graph-epoch boundary: player={} graphEpoch={} rejected={}",
							update.playerID,
							static_cast<unsigned long long>(newEpoch),
							removed);
					}
				}
				puppet->second.target = update.transform;
				puppet->second.lastSequence = update.transform.sequence;
				if (update.transform.furnitureRefID != 0 &&
					!puppet->second.pendingAnimationActions.empty()) {
					logs::info(
						"Animation sync discarded queued attacks at furniture checkpoint: player={} furniture={:08X} rejected={}",
						update.playerID,
						update.transform.furnitureRefID,
						puppet->second.pendingAnimationActions.size());
					puppet->second.pendingAnimationActions.clear();
				}
			}

			for (auto& update : changes.appearances) {
				// An appearance may arrive before the first checkpoint. The puppet is
				// only spawned once a checkpoint places it in the local space.
				auto& puppet = a_puppets[update.playerID];
				PlayerAppearance::AcceptRemoteAppearance(
					puppet.appearance,
					update.playerID,
					std::move(update.appearance));
			}

			for (auto& update : changes.equipment) {
				// Equipment is kept independent of whether a puppet reference exists,
				// so every respawn reconciles from the newest revision.
				auto& puppet = a_puppets[update.playerID];
				PlayerEquipment::AcceptRemoteEquipment(
					puppet.equipment,
					update.playerID,
					std::move(update.equipment));
			}

			for (auto& action : changes.animationActions) {
				const auto puppet = a_puppets.find(action.playerID);
				if (puppet == a_puppets.end() ||
					action.graphEpoch < puppet->second.target.graphEpoch ||
					puppet->second.target.furnitureRefID != 0) {
					logs::info(
						"Animation sync rejected action without a compatible checkpoint: player={} actionSequence={} graphEpoch={}",
						action.playerID,
						static_cast<unsigned long long>(action.actionSequence),
						static_cast<unsigned long long>(action.graphEpoch));
					continue;
				}
				if (puppet->second.pendingAnimationActions.size() >=
					kMaximumQueuedAnimationActions) {
					logs::warn(
						"Animation sync rejected action because the bounded queue is full: player={} actionSequence={}",
						action.playerID,
						static_cast<unsigned long long>(action.actionSequence));
					continue;
				}
				puppet->second.pendingAnimationActions.push_back(
					PendingAnimationAction{ .action = std::move(action) });
			}
		}

		void AgeAnimationActions(
			const std::string& a_playerID,
			RemotePuppet& a_puppet,
			float a_delta)
		{
			for (auto& pending : a_puppet.pendingAnimationActions) {
				pending.age += (std::max)(a_delta, 0.0F);
				if (pending.dispatchAttempts != 0) {
					pending.retryElapsed += (std::max)(a_delta, 0.0F);
				}
			}
			while (!a_puppet.pendingAnimationActions.empty() &&
				a_puppet.pendingAnimationActions.front().age >=
					kAnimationActionTimeout) {
				logs::warn(
					"Animation sync timed out queued attack: player={} actionSequence={} graphEpoch={}",
					a_playerID,
					static_cast<unsigned long long>(
						a_puppet.pendingAnimationActions.front().action.actionSequence),
					static_cast<unsigned long long>(
						a_puppet.pendingAnimationActions.front().action.graphEpoch));
				a_puppet.pendingAnimationActions.pop_front();
			}
		}

		void AdvanceAnimationAction(
			const std::string& a_playerID,
			RemotePuppet& a_puppet)
		{
			if (a_puppet.pendingAnimationActions.empty() ||
				!a_puppet.controlledNPC ||
				NPCSpawner::IsAnimationActionPlaying(*a_puppet.controlledNPC)) {
				return;
			}

			auto& pending = a_puppet.pendingAnimationActions.front();
			const auto& action = pending.action;
			if (action.graphEpoch != a_puppet.target.graphEpoch ||
				action.checkpointSequence > a_puppet.lastSequence ||
				a_puppet.target.animationState.jumping ||
				a_puppet.target.furnitureRefID != 0 ||
				!a_puppet.target.animationState.weaponDrawn ||
				// A weapon swap published before the attack must reach the hands first.
				!PlayerEquipment::AreHandsSettled(a_puppet.equipment) ||
				!NPCSpawner::CanDispatchNormalAttack(*a_puppet.controlledNPC)) {
				return;
			}

			++pending.dispatchAttempts;
			const bool accepted = NPCSpawner::DispatchNormalAttack(
				*a_puppet.controlledNPC);
			if (accepted) {
				logs::info(
					"Animation sync processed normal attack: player={} actor={:08X} actionSequence={} graphEpoch={} checkpointSequence={}",
					a_playerID,
					a_puppet.controlledNPC->handle.native_handle(),
					static_cast<unsigned long long>(action.actionSequence),
					static_cast<unsigned long long>(action.graphEpoch),
					static_cast<unsigned long long>(action.checkpointSequence));
				a_puppet.pendingAnimationActions.pop_front();
			} else if (pending.dispatchAttempts >=
					kMaximumAnimationActionAttempts &&
				pending.retryElapsed >= kMinimumAnimationActionRetrySeconds) {
				logs::warn(
					"Animation sync rejected normal attack after bounded retries: player={} actor={:08X} actionSequence={} attempts={} retrySeconds={:.3f}",
					a_playerID,
					a_puppet.controlledNPC->handle.native_handle(),
					static_cast<unsigned long long>(action.actionSequence),
					pending.dispatchAttempts,
					pending.retryElapsed);
				a_puppet.pendingAnimationActions.pop_front();
			}
		}

		// The latest checkpoint's animation state, except that a draw waits for the
		// synchronized weapon to reach the hands so the puppet does not draw fists
		// and then pop the weapon in. The wait is bounded by PlayerEquipment.
		Multiplayer::AnimationState GetPresentableAnimationState(const RemotePuppet& a_puppet)
		{
			auto target = a_puppet.target.animationState;
			if (a_puppet.controlledNPC && target.weaponDrawn &&
				!a_puppet.controlledNPC->targetAnimationState.weaponDrawn &&
				!PlayerEquipment::IsReadyForWeaponDraw(a_puppet.equipment)) {
				target.weaponDrawn = false;
			}
			return target;
		}

		void AdvanceAnimationState(
			const std::string& a_playerID,
			RemotePuppet& a_puppet)
		{
			if (!a_puppet.controlledNPC ||
				NPCSpawner::IsFurnitureInteractionPlaying(
					*a_puppet.controlledNPC) ||
				!NPCSpawner::IsAnimationStateSettledExceptLanding(
					*a_puppet.controlledNPC)) {
				return;
			}

			const auto current = a_puppet.controlledNPC->targetAnimationState;
			const auto target = GetPresentableAnimationState(a_puppet);
			if (current == target) {
				return;
			}
			if (!NPCSpawner::IsAnimationStateSettled(*a_puppet.controlledNPC) &&
				!target.jumping) {
				// Only a consecutive takeoff may interrupt a landing branch. Any other
				// change waits for JumpLandEnd and the grounded locomotion reassertion.
				return;
			}

			const auto next = GetNextAnimationStep(current, target);
			if (next == current) {
				return;
			}
			logs::info(
				"Animation sync advancing animation state: player={} actor={:08X} sequence={} current={}/{} step={}/{} target={}/{}",
				a_playerID,
				a_puppet.controlledNPC->handle.native_handle(),
				static_cast<unsigned long long>(a_puppet.lastSequence),
				Multiplayer::GetMovementStateName(current.movementState),
				current.jumping ? "jumping" : "grounded",
				Multiplayer::GetMovementStateName(next.movementState),
				next.jumping ? "jumping" : "grounded",
				Multiplayer::GetMovementStateName(target.movementState),
				target.jumping ? "jumping" : "grounded");
			NPCSpawner::SetAnimationState(*a_puppet.controlledNPC, next);
		}

		void UpdateRemotePuppet(
			const std::string& a_playerID,
			RemotePuppet& a_puppet,
			RE::TESNPC* a_playerBase,
			RE::FormID a_localCellID,
			RE::FormID a_localWorldspaceID,
			float a_delta)
		{
			const bool isInLocalSpace = CanSynchronizeInLocalSpace(
				a_puppet.target,
				a_localCellID,
				a_localWorldspaceID);
			const bool changedSpawnedSpace = a_puppet.controlledNPC &&
				!IsSameCoordinateSpace(
					a_puppet.target.cellID,
					a_puppet.target.worldspaceID,
					a_puppet.spawnedCellID,
					a_puppet.spawnedWorldspaceID);
			PlayerAppearance::AdvanceRemoteAppearance(a_puppet.appearance, a_delta);
			if (a_puppet.controlledNPC && isInLocalSpace && !changedSpawnedSpace &&
				PlayerAppearance::RequiresRespawn(a_puppet.appearance)) {
				// Race, sex, head parts, and weight are only applied when 3D loads.
				// Rebuild the puppet from the newest checkpoint with the new base.
				const auto actorHandle =
					a_puppet.controlledNPC->handle.native_handle();
				NPCSpawner::RemoveControlledNPC(*a_puppet.controlledNPC);
				a_puppet.controlledNPC.reset();
				a_puppet.pendingAnimationActions.clear();
				a_puppet.appliedSequence = 0;
				a_puppet.spawnRetryRemaining = 0.0F;
				logs::info(
					"Recycling remote player puppet {} actor {:08X} after appearance change: spawnedRevision={} revision={}",
					a_playerID,
					actorHandle,
					static_cast<unsigned long long>(a_puppet.appearance.spawnedRevision),
					static_cast<unsigned long long>(a_puppet.appearance.latest->revision));
			}
			if (a_puppet.controlledNPC &&
				(!isInLocalSpace || changedSpawnedSpace)) {
				const auto actorHandle =
					a_puppet.controlledNPC->handle.native_handle();
				NPCSpawner::RemoveControlledNPC(*a_puppet.controlledNPC);
				a_puppet.controlledNPC.reset();
				a_puppet.pendingAnimationActions.clear();
				a_puppet.appliedSequence = 0;
				a_puppet.spawnRetryRemaining = 0.0F;
				logs::info(
					"Recycling remote player puppet {} actor {:08X} after coordinate-space change: spawnedCell={:08X} spawnedWorld={:08X} targetCell={:08X} targetWorld={:08X} localCell={:08X} localWorld={:08X}",
					a_playerID,
					actorHandle,
					a_puppet.spawnedCellID,
					a_puppet.spawnedWorldspaceID,
					a_puppet.target.cellID,
					a_puppet.target.worldspaceID,
					a_localCellID,
					a_localWorldspaceID);
			}

			AgeAnimationActions(a_playerID, a_puppet, a_delta);
			if (!isInLocalSpace) {
				// Do not preserve transient combat history while no puppet/checkpoint
				// can be reconstructed in the local coordinate space.
				a_puppet.pendingAnimationActions.clear();
				return;
			}

			const RE::NiPoint3 targetPosition{
				a_puppet.target.x,
				a_puppet.target.y,
				a_puppet.target.z
			};

			if (a_puppet.controlledNPC &&
				a_puppet.appliedSequence != a_puppet.lastSequence) {
				NPCSpawner::SetTargetTransform(
					*a_puppet.controlledNPC,
					targetPosition,
					a_puppet.target.heading,
					a_puppet.target.speed,
					a_puppet.target.direction);
				a_puppet.appliedSequence = a_puppet.lastSequence;
			}

			if (a_puppet.controlledNPC) {
				auto& controlledNPC = *a_puppet.controlledNPC;
				PlayerEquipment::UpdateSpawnedPuppet(
					a_puppet.equipment,
					a_playerID,
					controlledNPC,
					a_delta);
				bool furniturePlaying =
					NPCSpawner::IsFurnitureInteractionPlaying(controlledNPC);
				if (controlledNPC.targetFurnitureRefID !=
						a_puppet.target.furnitureRefID &&
					(a_puppet.target.furnitureRefID == 0 || furniturePlaying)) {
					// Exit and direct furniture-to-furniture changes must update the
					// authoritative target immediately. The native lifecycle exits the
					// old marker before entering a replacement.
					NPCSpawner::SetFurnitureState(
						controlledNPC,
						a_puppet.target.furnitureRefID);
					furniturePlaying =
						NPCSpawner::IsFurnitureInteractionPlaying(controlledNPC);
				}

				const bool attackPlaying =
					NPCSpawner::IsAnimationActionPlaying(controlledNPC);
				if (!attackPlaying && !furniturePlaying) {
					AdvanceAnimationState(a_playerID, a_puppet);
				}

				if (a_puppet.target.furnitureRefID != 0 &&
					controlledNPC.targetFurnitureRefID !=
						a_puppet.target.furnitureRefID &&
					!attackPlaying &&
					!controlledNPC.targetAnimationState.jumping &&
					NPCSpawner::IsAnimationStateSettled(controlledNPC)) {
					// First entry waits until the checkpoint's grounded locomotion and
					// weapon presentation have settled. Once assigned, furniture owns
					// graph and transform progression until exit completes.
					NPCSpawner::SetFurnitureState(
						controlledNPC,
						a_puppet.target.furnitureRefID);
					furniturePlaying =
						NPCSpawner::IsFurnitureInteractionPlaying(controlledNPC);
				}
				// Stage the freshest base state before considering an action. If this
				// changes the graph request, CanDispatchNormalAttack keeps the action
				// queued until ApplyAnimationUpdate settles it on a following frame.
				if (!furniturePlaying) {
					AdvanceAnimationAction(a_playerID, a_puppet);
				}
				const auto presentableState = GetPresentableAnimationState(a_puppet);
				std::optional<Multiplayer::AnimationState> translationState;
				if (!furniturePlaying) {
					if (controlledNPC.targetAnimationState == presentableState) {
						translationState = presentableState;
					} else if (NPCSpawner::IsAnimationActionPlaying(controlledNPC) ||
						(controlledNPC.targetAnimationState.jumping &&
							presentableState.jumping)) {
						// An attack clip or an airborne branch holds the dispatched
						// locomotion for its whole duration, while the remote player keeps
						// travelling. Follow the latest sample instead of freezing and then
						// snapping once the graph releases.
						translationState = presentableState;
					}
				}
				if (!NPCSpawner::UpdateControlledNPC(
						controlledNPC,
						a_delta,
						translationState)) {
					logs::warn(
						"Remote player puppet {} lost its actor handle; respawning it",
						a_playerID);
					a_puppet.controlledNPC.reset();
					a_puppet.pendingAnimationActions.clear();
					a_puppet.spawnRetryRemaining = kSpawnRetryDelay;
				} else if (auto actor = controlledNPC.handle.get()) {
					PlayerAppearance::UpdateSpawnedPuppet(
						a_puppet.appearance,
						a_playerID,
						*actor,
						controlledNPC.ready);
				}
			}

			if (a_puppet.controlledNPC) {
				return;
			}

			a_puppet.spawnRetryRemaining -= (std::max)(a_delta, 0.0F);
			if (a_puppet.spawnRetryRemaining > 0.0F) {
				return;
			}

			auto* spawnBase = PlayerAppearance::GetSpawnBase(
				a_puppet.appearance,
				a_playerID,
				a_playerBase);
			if (!spawnBase) {
				// Briefly wait for the first appearance so a late joiner is not spawned
				// with the local look and immediately replaced.
				return;
			}

			a_puppet.controlledNPC = NPCSpawner::SpawnControlledNPC(
				spawnBase,
				targetPosition,
				a_puppet.target.heading,
				Multiplayer::AnimationState{},
				kPuppetSpeed,
				kPuppetStoppingDistance);
			if (!a_puppet.controlledNPC) {
				a_puppet.spawnRetryRemaining = kSpawnRetryDelay;
				return;
			}
			PlayerAppearance::OnPuppetSpawned(a_puppet.appearance, spawnBase);
			PlayerEquipment::OnPuppetSpawned(a_puppet.equipment);
			a_puppet.appliedSequence = a_puppet.lastSequence;
			a_puppet.spawnedCellID = a_puppet.target.cellID;
			a_puppet.spawnedWorldspaceID = a_puppet.target.worldspaceID;

			logs::info(
				"Allocated remote player puppet {} as actor {:08X} at transform sequence={} with target animation state={}/{}; waiting for 3D and animation graph readiness",
				a_playerID,
				a_puppet.controlledNPC->handle.native_handle(),
				static_cast<unsigned long long>(a_puppet.appliedSequence),
				Multiplayer::GetMovementStateName(
					a_puppet.target.animationState.movementState),
				a_puppet.target.animationState.jumping ? "jumping" : "grounded");

			if (auto* console = RE::ConsoleLog::GetSingleton()) {
				console->Print(
					"SkyrimMP remote player puppet reference allocated; initializing.");
			}
		}

		void OnFrame(RE::PlayerCharacter* a_player, float a_delta)
		{
			static RemotePuppets remotePuppets;

			ApplyNetworkChanges(remotePuppets);
			if (!a_player) {
				return;
			}

			bool localGraphEpochChanged = false;
			const std::uint32_t capturedNormalAttacks =
				ObserveLocalNormalAttack(*a_player) ? 1 : 0;
			const auto localGraphEpoch = ObserveLocalGraphEpoch(
				*a_player,
				localGraphEpochChanged);
			// Flush equipment before a captured attack can be published, so peers
			// receive a weapon swap ahead of the attack that uses it.
			PlayerEquipment::UpdateLocalEquipment(
				*a_player,
				a_delta,
				capturedNormalAttacks != 0);
			PublishLocalTransform(
				*a_player,
				a_delta,
				localGraphEpoch,
				localGraphEpochChanged,
				capturedNormalAttacks);
			PlayerAppearance::UpdateLocalAppearance(*a_player, a_delta);

			auto* localCell = a_player->GetParentCell();
			auto* playerBase = a_player->GetActorBase();
			if (!localCell || !playerBase) {
				return;
			}

			for (auto& [playerID, puppet] : remotePuppets) {
				UpdateRemotePuppet(
					playerID,
					puppet,
					playerBase,
					localCell->GetFormID(),
					GetWorldspaceID(*a_player),
					a_delta);
			}
		}

		struct MainLoopUpdateHook
		{
			static void Thunk(std::int64_t a_context)
			{
				_original(a_context);

				const auto delta = RE::Main::QFrameAnimTime();
				if (!std::isfinite(delta) || delta <= 0.0F ||
					!NPCSpawner::BindAuthoritativeGameThread()) {
					return;
				}

				OnFrame(RE::PlayerCharacter::GetSingleton(), delta);
			}

			static inline REL::Relocation<decltype(Thunk)> _original;
		};
	}

	void Install()
	{
		// Remote-replica entry detours use the trampoline too, so allocate first.
		SKSE::AllocTrampoline(14 + NPCSpawner::kRemoteReplicaTrampolineSize);
		NPCSpawner::InstallRemoteReplicaHooks();

		REL::Relocation<std::uintptr_t> mainLoopUpdateCall{
			RELOCATION_ID(35565, 36564),
			REL::VariantOffset(0x53, 0x6E, 0x68)
		};
		MainLoopUpdateHook::_original = SKSE::GetTrampoline().write_call<5>(
			mainLoopUpdateCall.address(),
			MainLoopUpdateHook::Thunk);

		logs::info("Installed authoritative Skyrim main-loop network frame hook");
	}
}
