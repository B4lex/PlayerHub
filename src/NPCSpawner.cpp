#include "NPCSpawner.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <REX/W32/KERNEL32.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NPCSpawner
{
	namespace
	{
		constexpr float kArrivalTolerance = 1.0F;
		constexpr float kMaximumFrameTime = 0.1F;
		constexpr float kAirborneTransformCatchUpTime = 0.2F;
		constexpr std::uint32_t kMaximumEventDispatchAttempts = 4;
		// Rejected graph events are also retried for at least this long, so a high
		// frame rate cannot spend every attempt inside one short graph blend.
		constexpr float kMinimumEventRetrySeconds = 0.1F;
		constexpr std::uint32_t kInitialBlockedEventLogs = 8;
		// Skyrim cross-blends directional locomotion (CyclicFreeze, then
		// CyclicCrossBlend) instead of sweeping through intermediate directions.
		// The source's Direction ramps over several samples, so only a change of
		// at least 1/8 of a turn starts one, and never before the previous
		// 0.2-second cross-blend has finished. Smaller changes follow Direction.
		constexpr float kDirectionCrossBlendThreshold = 0.125F;
		constexpr float kDirectionCrossBlendIntervalSeconds = 0.25F;
		constexpr std::uint32_t kBlockedEventLogInterval = 120;
		constexpr float kHitReactionObservationSeconds = 0.1F;
		constexpr std::uint32_t kGraphRecoveryStableFrames = 2;
		constexpr float kGraphRecoveryFallbackSeconds = 2.0F;
		constexpr std::uint32_t kGraphRecoveryWaitLogInterval = 120;
		constexpr std::uint32_t kActorInitializationLogInterval = 120;
		constexpr float kLandingCompletionFallbackSeconds = 2.0F;
		// Measured in time, not frames: a one-handed sword draw takes about 1.4
		// seconds, which a frame budget cuts short at high frame rates.
		constexpr float kWeaponTransitionFallbackSeconds = 2.5F;
		constexpr float kAttackStartFallbackSeconds = 1.25F;
		constexpr float kAttackPlaybackFallbackSeconds = 4.0F;
		constexpr float kFurnitureRetryInterval = 1.0F;
		constexpr float kFurnitureEntryFallbackSeconds = 10.0F;
		constexpr float kFurnitureExitFallbackSeconds = 10.0F;
		constexpr float kFurnitureEntryAlignmentDistance = 24.0F;
		// Native entry aligns the actor to the chosen entry point itself; this only
		// bounds how far that alignment may glide the puppet.
		constexpr float kFurnitureEntryMarkerDistance = 96.0F;
		constexpr float kTwoPi = 6.28318530717958647692F;
		constexpr float kHeadingDeadZone = 0.00872664625997164788F;
		constexpr float kMaximumHeadingRate = kTwoPi;
		constexpr bool IsClientManagedAnimationEvent(
			std::string_view a_eventName) noexcept
		{
			return a_eventName == "SwimStart" ||
				a_eventName == "swimStop" ||
				a_eventName == "WeapEquip" ||
				a_eventName == "WeapUnequip" ||
				a_eventName == "Equip" ||
				a_eventName == "Unequip";
		}

		static_assert(IsClientManagedAnimationEvent("swimStop"));
		static_assert(IsClientManagedAnimationEvent("SwimStart"));
		static_assert(IsClientManagedAnimationEvent("WeapEquip"));
		static_assert(IsClientManagedAnimationEvent("WeapUnequip"));
		static_assert(!IsClientManagedAnimationEvent("AttackStart"));
		constexpr bool IsWeaponTransitionAnimating(
			RE::WEAPON_STATE a_weaponState,
			bool a_weaponDrawn) noexcept
		{
			return a_weaponDrawn ?
				a_weaponState == RE::WEAPON_STATE::kDrawing :
				a_weaponState == RE::WEAPON_STATE::kSheathing;
		}

		static_assert(IsWeaponTransitionAnimating(
			RE::WEAPON_STATE::kDrawing,
			true));
		static_assert(IsWeaponTransitionAnimating(
			RE::WEAPON_STATE::kSheathing,
			false));
		static_assert(!IsWeaponTransitionAnimating(
			RE::WEAPON_STATE::kWantToDraw,
			true));
		static_assert(!IsWeaponTransitionAnimating(
			RE::WEAPON_STATE::kWantToSheathe,
			false));
		constexpr bool IsWeaponTransitionTraceEvent(
			std::string_view a_eventName) noexcept
		{
			return a_eventName.find("Equip") != std::string_view::npos ||
				a_eventName.find("equip") != std::string_view::npos ||
				a_eventName.find("Weapon") != std::string_view::npos ||
				a_eventName.find("weapon") != std::string_view::npos;
		}

		static_assert(IsWeaponTransitionTraceEvent("BeginWeaponSheathe"));
		static_assert(IsWeaponTransitionTraceEvent("Unequip_OutMoving"));
		static_assert(IsWeaponTransitionTraceEvent("weaponDraw"));
		static_assert(!IsWeaponTransitionTraceEvent("FootLeft"));
		constexpr bool IsWeaponTransitionEndEvent(
			std::string_view a_eventName,
			bool a_weaponDrawn) noexcept
		{
			return a_weaponDrawn ?
				(a_eventName == "WeapEquip_Out" ||
					a_eventName == "WeapEquip_OutMoving") :
				(a_eventName == "Unequip_Out" ||
					a_eventName == "Unequip_OutMoving");
		}

		static_assert(!IsWeaponTransitionEndEvent("weaponDraw", true));
		static_assert(IsWeaponTransitionEndEvent("WeapEquip_OutMoving", true));
		static_assert(!IsWeaponTransitionEndEvent("weaponSheathe", false));
		static_assert(IsWeaponTransitionEndEvent("Unequip_OutMoving", false));
		static_assert(!IsWeaponTransitionEndEvent("AttackStart", true));
		constexpr bool ShouldReassertAfterWeaponTransition(
			bool a_pending,
			bool a_completed,
			bool a_moving,
			bool a_jumping) noexcept
		{
			return a_pending && a_completed && (a_moving || a_jumping);
		}

		static_assert(ShouldReassertAfterWeaponTransition(true, true, true, false));
		static_assert(ShouldReassertAfterWeaponTransition(true, true, false, true));
		static_assert(!ShouldReassertAfterWeaponTransition(true, true, false, false));
		static_assert(!ShouldReassertAfterWeaponTransition(true, false, true, false));
		constexpr Multiplayer::AnimationState GetWeaponReplayBaseState(
			Multiplayer::AnimationState a_state) noexcept
		{
			a_state.jumping = false;
			switch (a_state.movementState) {
			case Multiplayer::MovementState::kSprinting:
				a_state.movementState = Multiplayer::MovementState::kRunning;
				break;
			case Multiplayer::MovementState::kSneakingWalking:
			case Multiplayer::MovementState::kSneakingRunning:
				a_state.movementState = Multiplayer::MovementState::kSneakingIdle;
				break;
			default:
				break;
			}
			return a_state;
		}

		static_assert(GetWeaponReplayBaseState(Multiplayer::AnimationState{
			.movementState = Multiplayer::MovementState::kSprinting,
			.jumping = true }) == Multiplayer::AnimationState{
			.movementState = Multiplayer::MovementState::kRunning });
		// PlayerBase (00000007) is not an ordinary placeable NPC record. Use a
		// known-good actor base only to make CreateReferenceAtLocation construct the
		// reference, then replace it with PlayerBase befor`e 3D is initialized.
		constexpr RE::FormID kActorConstructionTemplateFormID = 0x00013BBF;

		constexpr bool IsMovingState(Multiplayer::MovementState a_state)
		{
			return a_state != Multiplayer::MovementState::kIdle &&
				a_state != Multiplayer::MovementState::kSneakingIdle;
		}

		struct PuppetGraphRegistration
		{
			const RE::Actor* actor{ nullptr };
			const RE::MagicTarget* magicTarget{ nullptr };
			const RE::ActorValueOwner* actorValueOwner{ nullptr };
			std::uint32_t actorHandle{ 0 };
			std::unordered_map<std::string, std::uint32_t> blockedEventCounts;
			std::uint32_t blockedMagicEffects{ 0 };
			std::uint32_t blockedHealthDamage{ 0 };
			std::uint32_t pendingHitObservations{ 0 };
			std::uint32_t pendingCombatSanitizations{ 0 };
			std::uint32_t pendingJumpLandEnds{ 0 };
			std::uint32_t pendingSwimStops{ 0 };
			std::uint32_t pendingWeaponTransitionEnds{ 0 };
			std::uint32_t hitObservationCount{ 0 };
			std::uint32_t combatSanitizationCount{ 0 };
			std::uint32_t jumpLandEndCount{ 0 };
			std::uint32_t weaponTransitionEndCount{ 0 };
			bool authoritativeJumping{ false };
			// Network travel direction while moving, in graph Direction units. The
			// actor's direction channel would otherwise feed the puppet's own (empty)
			// movement into the graph on every animation update.
			float authoritativeDirection{ 0.0F };
			bool authoritativeWeaponDrawn{ false };
			bool animationFirewallEnabled{ false };
			bool furnitureAnimationOwned{ false };
			bool landingCompletionObservationAllowed{ false };
			bool observeJumpLandEndEvent{ false };
			bool observeWeaponTransitionEndEvent{ false };
		};

		std::mutex s_puppetGraphMutex;
		std::unordered_map<
			const RE::IAnimationGraphManagerHolder*,
			PuppetGraphRegistration>
			s_puppetGraphs;
		std::atomic_uint32_t s_authoritativeGameThreadID{ 0 };
		std::atomic_bool s_wrongThreadLogged{ false };
		std::atomic_bool s_gameThreadMigrationLogged{ false };

		class PuppetAnimationGraphEventSink final :
			public RE::BSTEventSink<RE::BSAnimationGraphEvent>
		{
		public:
			static PuppetAnimationGraphEventSink* GetSingleton()
			{
				static PuppetAnimationGraphEventSink singleton;
				return std::addressof(singleton);
			}

			RE::BSEventNotifyControl ProcessEvent(
				const RE::BSAnimationGraphEvent* a_event,
				RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
			{
				const auto* eventName = a_event ? a_event->tag.c_str() : nullptr;
				if (!a_event || !a_event->holder || !eventName) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const std::string_view eventView{ eventName };
				const bool isJumpLandEnd = eventView == "JumpLandEnd";
				const bool isWeaponTransitionEnd =
					IsWeaponTransitionEndEvent(eventView, true) ||
					IsWeaponTransitionEndEvent(eventView, false);
				const bool shouldTraceWeaponEvent =
					IsWeaponTransitionTraceEvent(eventView);
				if (!isJumpLandEnd && !isWeaponTransitionEnd &&
					!shouldTraceWeaponEvent) {
					return RE::BSEventNotifyControl::kContinue;
				}

				std::uint32_t actorHandle = 0;
				std::uint32_t landingCompletionCount = 0;
				std::uint32_t weaponCompletionCount = 0;
				bool traceWeaponEvent = false;
				bool expectedWeaponDrawn = false;
				{
					std::scoped_lock lock{ s_puppetGraphMutex };
					const auto puppet = std::ranges::find_if(
						s_puppetGraphs,
						[a_event](const auto& a_entry) {
							return a_entry.second.actor == a_event->holder;
						});
					if (puppet != s_puppetGraphs.end() && isJumpLandEnd &&
						puppet->second.landingCompletionObservationAllowed &&
						puppet->second.observeJumpLandEndEvent) {
						++puppet->second.pendingJumpLandEnds;
						landingCompletionCount =
							++puppet->second.jumpLandEndCount;
						puppet->second.observeJumpLandEndEvent = false;
						actorHandle = puppet->second.actorHandle;
					}
					if (puppet != s_puppetGraphs.end() &&
						puppet->second.observeWeaponTransitionEndEvent &&
						shouldTraceWeaponEvent) {
						traceWeaponEvent = true;
						expectedWeaponDrawn =
							puppet->second.authoritativeWeaponDrawn;
						actorHandle = puppet->second.actorHandle;
					}
					if (puppet != s_puppetGraphs.end() &&
						puppet->second.observeWeaponTransitionEndEvent &&
						IsWeaponTransitionEndEvent(
							eventView,
							puppet->second.authoritativeWeaponDrawn)) {
						++puppet->second.pendingWeaponTransitionEnds;
						weaponCompletionCount =
							++puppet->second.weaponTransitionEndCount;
						puppet->second.observeWeaponTransitionEndEvent = false;
						actorHandle = puppet->second.actorHandle;
					}
				}

				if (landingCompletionCount != 0) {
					logs::info(
						"Observed puppet landing completion marker: actor={:08X} count={}",
						actorHandle,
						landingCompletionCount);
				}
				if (traceWeaponEvent) {
					logs::info(
						"Observed puppet weapon-transition graph event: actor={:08X} expectedWeaponDrawn={} event={}",
						actorHandle,
						expectedWeaponDrawn,
						eventName);
				}
				if (weaponCompletionCount != 0) {
					logs::info(
						"Observed puppet weapon transition completion marker: actor={:08X} event={} count={}",
						actorHandle,
						eventName,
						weaponCompletionCount);
				}
				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			PuppetAnimationGraphEventSink() = default;
		};

		bool RegisterLandingCompletionEventSink(
			RE::BSAnimationGraphManager& a_graphManager)
		{
			if (a_graphManager.graphs.empty() || !a_graphManager.graphs.front()) {
				return false;
			}

			// Animation annotations are emitted by the graph's event source. They do
			// not pass through IAnimationGraphManagerHolder::NotifyAnimationGraph,
			// which is the input path used by the puppet firewall below.
			a_graphManager.graphs.front()
				->GetEventSource<RE::BSAnimationGraphEvent>()
				->AddEventSink(PuppetAnimationGraphEventSink::GetSingleton());
			return true;
		}

		bool IsAuthoritativeGameThread()
		{
			const auto authoritativeThread = s_authoritativeGameThreadID.load(
				std::memory_order_acquire);
			return authoritativeThread != 0 &&
				authoritativeThread == REX::W32::GetCurrentThreadId();
		}

		bool RequireGameThread(const char* a_operation)
		{
			const auto currentThread = REX::W32::GetCurrentThreadId();
			const auto authoritativeThread = s_authoritativeGameThreadID.load(
				std::memory_order_acquire);
			if (authoritativeThread != 0 &&
				authoritativeThread == currentThread) {
				return true;
			}

			if (!s_wrongThreadLogged.exchange(true, std::memory_order_relaxed)) {
				logs::critical(
					"Refused remote-puppet engine access outside the authoritative main-loop thread: operation={} currentThread={} authoritativeThread={}",
					a_operation,
					currentThread,
					authoritativeThread);
			}
			return false;
		}

		void RegisterPuppetGraph(ControlledNPC& a_npc, RE::Actor& a_actor)
		{
			auto* holder = static_cast<RE::IAnimationGraphManagerHolder*>(
				std::addressof(a_actor));
			if (a_npc.animationGraphHolder == holder) {
				return;
			}

			std::scoped_lock lock{ s_puppetGraphMutex };
			if (a_npc.animationGraphHolder) {
				s_puppetGraphs.erase(a_npc.animationGraphHolder);
			}
			s_puppetGraphs.insert_or_assign(
				holder,
				PuppetGraphRegistration{
					.actor = std::addressof(a_actor),
					.magicTarget = a_actor.AsMagicTarget(),
					.actorValueOwner = a_actor.AsActorValueOwner(),
					.actorHandle = a_npc.handle.native_handle(),
					.authoritativeJumping =
						a_npc.targetAnimationState.jumping,
					.authoritativeDirection =
						IsMovingState(a_npc.targetAnimationState.movementState) ?
							a_npc.animationDirection : 0.0F,
					.authoritativeWeaponDrawn =
						a_npc.targetAnimationState.weaponDrawn,
					.animationFirewallEnabled = false,
					.furnitureAnimationOwned = a_npc.animationState &&
						a_npc.animationState->furnitureAnimationOwned,
					.observeWeaponTransitionEndEvent =
						a_npc.targetAnimationState.weaponDrawn
				});
			a_npc.animationGraphHolder = holder;
		}

		void EnablePuppetAnimationFirewall(ControlledNPC& a_npc)
		{
			bool activated = false;
			{
				std::scoped_lock lock{ s_puppetGraphMutex };
				const auto puppet = s_puppetGraphs.find(a_npc.animationGraphHolder);
				if (puppet != s_puppetGraphs.end() &&
					!puppet->second.animationFirewallEnabled) {
					puppet->second.animationFirewallEnabled = true;
					activated = true;
				}
			}

			if (activated) {
				logs::info(
					"Activated puppet animation firewall: actor={:08X}",
					a_npc.handle.native_handle());
			}
		}

		void DisablePuppetAnimationFirewall(ControlledNPC& a_npc)
		{
			std::scoped_lock lock{ s_puppetGraphMutex };
			const auto puppet = s_puppetGraphs.find(a_npc.animationGraphHolder);
			if (puppet != s_puppetGraphs.end()) {
				puppet->second.animationFirewallEnabled = false;
			}
		}

		void SetFurnitureAnimationOwnership(
			ControlledNPC& a_npc,
			bool a_owned)
		{
			if (a_npc.animationState) {
				a_npc.animationState->furnitureAnimationOwned = a_owned;
			}

			std::scoped_lock lock{ s_puppetGraphMutex };
			const auto puppet = s_puppetGraphs.find(a_npc.animationGraphHolder);
			if (puppet != s_puppetGraphs.end()) {
				puppet->second.furnitureAnimationOwned = a_owned;
			}
		}

		void UnregisterPuppetGraph(ControlledNPC& a_npc)
		{
			if (!a_npc.animationGraphHolder) {
				return;
			}

			std::scoped_lock lock{ s_puppetGraphMutex };
			s_puppetGraphs.erase(a_npc.animationGraphHolder);
			a_npc.animationGraphHolder = nullptr;
		}

		std::optional<bool> GetAuthoritativeJumpState(const RE::Actor* a_actor)
		{
			if (!a_actor) {
				return std::nullopt;
			}

			std::scoped_lock lock{ s_puppetGraphMutex };
			const auto puppet = std::ranges::find_if(
				s_puppetGraphs,
				[a_actor](const auto& a_entry) {
					return a_entry.second.actor == a_actor;
				});
			if (puppet == s_puppetGraphs.end()) {
				return std::nullopt;
			}

			return puppet->second.authoritativeJumping;
		}

		void SetAuthoritativeJumpState(
			const ControlledNPC& a_npc,
			bool a_jumping)
		{
			std::scoped_lock lock{ s_puppetGraphMutex };
			const auto puppet = s_puppetGraphs.find(a_npc.animationGraphHolder);
			if (puppet != s_puppetGraphs.end()) {
				puppet->second.authoritativeJumping = a_jumping;
			}
		}

		void SetAuthoritativeDirection(const ControlledNPC& a_npc)
		{
			const float direction =
				IsMovingState(a_npc.targetAnimationState.movementState) ?
					a_npc.animationDirection : 0.0F;
			std::scoped_lock lock{ s_puppetGraphMutex };
			const auto puppet = s_puppetGraphs.find(a_npc.animationGraphHolder);
			if (puppet != s_puppetGraphs.end()) {
				puppet->second.authoritativeDirection = direction;
			}
		}

		std::optional<float> GetAuthoritativeDirection(const RE::Actor* a_actor)
		{
			if (!a_actor) {
				return std::nullopt;
			}

			std::scoped_lock lock{ s_puppetGraphMutex };
			const auto puppet = std::ranges::find_if(
				s_puppetGraphs,
				[a_actor](const auto& a_entry) {
					return a_entry.second.actor == a_actor;
				});
			return puppet != s_puppetGraphs.end() ?
				std::optional<float>{ puppet->second.authoritativeDirection } :
				std::nullopt;
		}

		void SetAuthoritativeWeaponDrawnState(
			const ControlledNPC& a_npc,
			bool a_weaponDrawn)
		{
			std::scoped_lock lock{ s_puppetGraphMutex };
			const auto puppet = s_puppetGraphs.find(a_npc.animationGraphHolder);
			if (puppet != s_puppetGraphs.end() &&
				puppet->second.authoritativeWeaponDrawn != a_weaponDrawn) {
				puppet->second.authoritativeWeaponDrawn = a_weaponDrawn;
				// Arm before DrawWeaponMagicHands runs on the next puppet update. Its
				// completion annotation may be emitted from an animation worker.
				puppet->second.pendingWeaponTransitionEnds = 0;
				puppet->second.observeWeaponTransitionEndEvent = true;
			}
		}

		std::optional<bool> GetAuthoritativeWeaponDrawnState(
			const RE::Actor& a_actor)
		{
			std::scoped_lock lock{ s_puppetGraphMutex };
			const auto puppet = std::ranges::find_if(
				s_puppetGraphs,
				[&a_actor](const auto& a_entry) {
					return a_entry.second.actor == std::addressof(a_actor);
				});
			return puppet != s_puppetGraphs.end() ?
				std::optional<bool>{ puppet->second.authoritativeWeaponDrawn } :
				std::nullopt;
		}

		bool ApplyAuthoritativeJumpState(RE::Actor& a_actor, bool a_jumping)
		{
			static const RE::BSFixedString inJumpStateVariable{ "bInJumpState" };
			return a_actor.SetGraphVariableBool(
				inJumpStateVariable,
				a_jumping);
		}

		bool ShouldSuppressRemotePuppetEffect(
			const RE::MagicItem* a_magicItem,
			const RE::Effect* a_effect)
		{
			// Effects propagated through projectiles or explosions do not always
			// retain a caster. Classify shouts from the originating MagicItem so they
			// can never alter a network-authoritative puppet.
			if (a_magicItem &&
				a_magicItem->GetSpellType() ==
					RE::MagicSystem::SpellType::kVoicePower) {
				return true;
			}

			if (!a_effect || !a_effect->baseEffect) {
				return false;
			}

			// Preserve benign buffs while rejecting harmful effects from every local
			// source, including NPCs, hazards, traps, and effects without a caster.
			return a_effect->IsHostile() || a_effect->baseEffect->IsDetrimental();
		}

		std::pair<std::uint32_t, std::uint32_t> BlockMagicEffect(
			RE::MagicTarget* a_target,
			const RE::MagicTarget::AddTargetData& a_targetData)
		{
			if (!a_target ||
				!ShouldSuppressRemotePuppetEffect(
					a_targetData.magicItem,
					a_targetData.effect)) {
				return {};
			}

			std::scoped_lock lock{ s_puppetGraphMutex };
			const auto puppet = std::ranges::find_if(
				s_puppetGraphs,
				[a_target](const auto& a_entry) {
					return a_entry.second.magicTarget == a_target;
				});
			if (puppet == s_puppetGraphs.end()) {
				return {};
			}

			return {
				puppet->second.actorHandle,
				++puppet->second.blockedMagicEffects
			};
		}

		std::uint32_t DispelSuppressedMagicEffects(RE::Actor& a_actor)
		{
			auto* magicTarget = a_actor.AsMagicTarget();
			if (!magicTarget) {
				return 0;
			}

			auto* activeEffects = magicTarget->GetActiveEffectList();
			if (!activeEffects) {
				return 0;
			}

			std::vector<RE::ActiveEffect*> effectsToDispel;
			for (auto* activeEffect : *activeEffects) {
				if (!activeEffect ||
					activeEffect->flags.any(RE::ActiveEffect::Flag::kDispelled)) {
					continue;
				}

				if (ShouldSuppressRemotePuppetEffect(
						activeEffect->spell,
						activeEffect->effect)) {
					effectsToDispel.push_back(activeEffect);
				}
			}

			for (auto* activeEffect : effectsToDispel) {
				const auto* baseEffect = activeEffect->GetBaseObject();
				logs::info(
					"Dispelling magic effect that bypassed the remote-puppet firewall: actor={:08X} spell={:08X} effect={:08X}",
					a_actor.GetHandle().native_handle(),
					activeEffect->spell ? activeEffect->spell->GetFormID() : 0,
					baseEffect ? baseEffect->GetFormID() : 0);
				activeEffect->Dispel(true);
			}

			return static_cast<std::uint32_t>(effectsToDispel.size());
		}

		std::pair<std::uint32_t, std::uint32_t> BlockHealthDamage(
			const RE::Actor* a_actor)
		{
			if (!a_actor) {
				return {};
			}

			std::scoped_lock lock{ s_puppetGraphMutex };
			const auto puppet = std::ranges::find_if(
				s_puppetGraphs,
				[a_actor](const auto& a_entry) {
					return a_entry.second.actor == a_actor;
				});
			if (puppet == s_puppetGraphs.end()) {
				return {};
			}

			return {
				puppet->second.actorHandle,
				++puppet->second.blockedHealthDamage
			};
		}

		std::pair<std::uint32_t, std::uint32_t> BlockHealthActorValueDamage(
			const RE::ActorValueOwner* a_actorValueOwner)
		{
			if (!a_actorValueOwner) {
				return {};
			}

			std::scoped_lock lock{ s_puppetGraphMutex };
			const auto puppet = std::ranges::find_if(
				s_puppetGraphs,
				[a_actorValueOwner](const auto& a_entry) {
					return a_entry.second.actorValueOwner == a_actorValueOwner;
				});
			if (puppet == s_puppetGraphs.end()) {
				return {};
			}

			return {
				puppet->second.actorHandle,
				++puppet->second.blockedHealthDamage
			};
		}

		void LogBlockedHealthDamage(
			std::uint32_t a_actorHandle,
			std::uint32_t a_blockedDamageCount,
			std::string_view a_path,
			float a_damage)
		{
			if (a_blockedDamageCount != 0 &&
				(a_blockedDamageCount <= kInitialBlockedEventLogs ||
					a_blockedDamageCount % kBlockedEventLogInterval == 0)) {
				logs::info(
					"Blocked local health damage to remote puppet: actor={:08X} path={} damage={:.3f} count={}",
					a_actorHandle,
					a_path,
					a_damage,
					a_blockedDamageCount);
			}
		}

		std::pair<std::uint32_t, std::uint32_t> RequestHitObservation(
			const RE::Actor* a_actor)
		{
			if (!a_actor) {
				return {};
			}

			std::scoped_lock lock{ s_puppetGraphMutex };
			const auto puppet = std::ranges::find_if(
				s_puppetGraphs,
				[a_actor](const auto& a_entry) {
					return a_entry.second.actor == a_actor;
				});
			if (puppet == s_puppetGraphs.end()) {
				return {};
			}

			++puppet->second.pendingHitObservations;
			return {
				puppet->second.actorHandle,
				++puppet->second.hitObservationCount
			};
		}

		std::uint32_t ConsumeHitObservations(const ControlledNPC& a_npc)
		{
			std::scoped_lock lock{ s_puppetGraphMutex };
			const auto puppet = s_puppetGraphs.find(a_npc.animationGraphHolder);
			if (puppet == s_puppetGraphs.end()) {
				return 0;
			}

			return std::exchange(puppet->second.pendingHitObservations, 0);
		}

		std::uint32_t ConsumeSwimStops(const ControlledNPC& a_npc)
		{
			std::scoped_lock lock{ s_puppetGraphMutex };
			const auto puppet = s_puppetGraphs.find(a_npc.animationGraphHolder);
			if (puppet == s_puppetGraphs.end()) {
				return 0;
			}

			return std::exchange(puppet->second.pendingSwimStops, 0);
		}

		std::uint32_t ConsumeWeaponTransitionEnds(const ControlledNPC& a_npc)
		{
			std::scoped_lock lock{ s_puppetGraphMutex };
			const auto puppet = s_puppetGraphs.find(a_npc.animationGraphHolder);
			if (puppet == s_puppetGraphs.end()) {
				return 0;
			}

			return std::exchange(
				puppet->second.pendingWeaponTransitionEnds,
				0);
		}

		void SetLandingCompletionObservation(
			const RE::Actor& a_actor,
			bool a_allowed)
		{
			std::scoped_lock lock{ s_puppetGraphMutex };
			const auto puppet = std::ranges::find_if(
				s_puppetGraphs,
				[&a_actor](const auto& a_entry) {
					return a_entry.second.actor == std::addressof(a_actor);
				});
			if (puppet != s_puppetGraphs.end()) {
				puppet->second.landingCompletionObservationAllowed = a_allowed;
				puppet->second.observeJumpLandEndEvent = false;
				puppet->second.pendingJumpLandEnds = 0;
			}
		}

		void ArmJumpLandEndObservation(const RE::Actor& a_actor)
		{
			std::scoped_lock lock{ s_puppetGraphMutex };
			const auto puppet = std::ranges::find_if(
				s_puppetGraphs,
				[&a_actor](const auto& a_entry) {
					return a_entry.second.actor == std::addressof(a_actor);
				});
			if (puppet != s_puppetGraphs.end() &&
				puppet->second.landingCompletionObservationAllowed) {
				// Arm only after the grounded transform and graph variables are in
				// place. A delayed marker from the prior jump must not complete this one.
				puppet->second.pendingJumpLandEnds = 0;
				puppet->second.observeJumpLandEndEvent = true;
			}
		}

		std::uint32_t ConsumeJumpLandEnds(const ControlledNPC& a_npc)
		{
			std::scoped_lock lock{ s_puppetGraphMutex };
			const auto puppet = s_puppetGraphs.find(a_npc.animationGraphHolder);
			if (puppet == s_puppetGraphs.end()) {
				return 0;
			}

			return std::exchange(puppet->second.pendingJumpLandEnds, 0);
		}

		std::uint32_t ConsumeCombatSanitizationRequests(const RE::Actor& a_actor)
		{
			std::scoped_lock lock{ s_puppetGraphMutex };
			const auto puppet = std::ranges::find_if(
				s_puppetGraphs,
				[&a_actor](const auto& a_entry) {
					return a_entry.second.actor == std::addressof(a_actor);
				});
			if (puppet == s_puppetGraphs.end()) {
				return 0;
			}

			return std::exchange(
				puppet->second.pendingCombatSanitizations,
				0);
		}

		std::pair<std::uint32_t, std::uint32_t> RecordCombatSanitization(
			const RE::Actor& a_actor)
		{
			std::scoped_lock lock{ s_puppetGraphMutex };
			const auto puppet = std::ranges::find_if(
				s_puppetGraphs,
				[&a_actor](const auto& a_entry) {
					return a_entry.second.actor == std::addressof(a_actor);
				});
			if (puppet == s_puppetGraphs.end()) {
				return {};
			}

			return {
				puppet->second.actorHandle,
				++puppet->second.combatSanitizationCount
			};
		}

		class PuppetHitEventSink final : public RE::BSTEventSink<RE::TESHitEvent>
		{
		public:
			static PuppetHitEventSink* GetSingleton()
			{
				static PuppetHitEventSink singleton;
				return std::addressof(singleton);
			}

			RE::BSEventNotifyControl ProcessEvent(
				const RE::TESHitEvent* a_event,
				RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				if (!a_event || !a_event->target) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* actor = a_event->target->As<RE::Actor>();
				const auto [actorHandle, requestCount] = RequestHitObservation(actor);
				if (requestCount != 0 &&
					(requestCount <= kInitialBlockedEventLogs ||
						requestCount % kBlockedEventLogInterval == 0)) {
					logs::info(
						"Observed local hit on remote puppet; watching for an animation reaction: actor={:08X} aggressor={:08X} source={:08X} projectile={:08X} count={}",
						actorHandle,
						a_event->cause ? a_event->cause->GetFormID() : 0,
						a_event->source,
						a_event->projectile,
						requestCount);
				}

				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			PuppetHitEventSink() = default;
		};

		class PuppetMagicEffectEventSink final :
			public RE::BSTEventSink<RE::TESMagicEffectApplyEvent>
		{
		public:
			static PuppetMagicEffectEventSink* GetSingleton()
			{
				static PuppetMagicEffectEventSink singleton;
				return std::addressof(singleton);
			}

			RE::BSEventNotifyControl ProcessEvent(
				const RE::TESMagicEffectApplyEvent* a_event,
				RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*) override
			{
				if (!a_event || !a_event->target) {
					return RE::BSEventNotifyControl::kContinue;
				}

				const auto* effect = RE::TESForm::LookupByID<RE::EffectSetting>(
					a_event->magicEffect);
				if (!effect || (!effect->IsHostile() && !effect->IsDetrimental())) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* actor = a_event->target->As<RE::Actor>();
				const auto [actorHandle, requestCount] = RequestHitObservation(actor);
				if (requestCount != 0 &&
					(requestCount <= kInitialBlockedEventLogs ||
						requestCount % kBlockedEventLogInterval == 0)) {
					logs::info(
						"Observed harmful magic effect on remote puppet; watching for an animation reaction: actor={:08X} caster={:08X} effect={:08X} count={}",
						actorHandle,
						a_event->caster ? a_event->caster->GetFormID() : 0,
						a_event->magicEffect,
						requestCount);
				}

				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			PuppetMagicEffectEventSink() = default;
		};

		struct PuppetMagicTargetHook
		{
			static bool Thunk(
				RE::MagicTarget* a_target,
				RE::MagicTarget::AddTargetData& a_targetData)
			{
				const auto [actorHandle, blockedEffectCount] =
					BlockMagicEffect(a_target, a_targetData);
				if (blockedEffectCount == 0) {
					return _original(a_target, a_targetData);
				}

				if (blockedEffectCount <= kInitialBlockedEventLogs ||
					blockedEffectCount % kBlockedEventLogInterval == 0) {
					logs::info(
						"Blocked magic effect before it reached remote puppet: actor={:08X} caster={:08X} spell={:08X} effect={:08X} count={}",
						actorHandle,
						a_targetData.caster ? a_targetData.caster->GetFormID() : 0,
						a_targetData.magicItem ?
							a_targetData.magicItem->GetFormID() :
							0,
						a_targetData.effect && a_targetData.effect->baseEffect ?
							a_targetData.effect->baseEffect->GetFormID() :
							0,
						blockedEffectCount);
				}

				// Reject the ActiveEffect itself so archetypes and scripts (for example
				// Ice Form's freeze) cannot change a network-authoritative puppet.
				return false;
			}

			static inline REL::Relocation<decltype(Thunk)> _original;
		};

		struct PuppetHealthDamageHook
		{
			static void Thunk(
				RE::Actor* a_actor,
				RE::Actor* a_attacker,
				float a_damage)
			{
				const auto [actorHandle, blockedDamageCount] =
					BlockHealthDamage(a_actor);
				if (blockedDamageCount != 0) {
					LogBlockedHealthDamage(
						actorHandle,
						blockedDamageCount,
						"HandleHealthDamage",
						a_damage);
					return;
				}

				_original(a_actor, a_attacker, a_damage);
			}

			static inline REL::Relocation<decltype(Thunk)> _original;
		};

		struct PuppetActorValueHook
		{
			static void Thunk(
				RE::ActorValueOwner* a_actorValueOwner,
				RE::ACTOR_VALUE_MODIFIER a_modifier,
				RE::ActorValue a_actorValue,
				float a_value)
			{
				if (a_modifier == RE::ACTOR_VALUE_MODIFIER::kDamage &&
					a_actorValue == RE::ActorValue::kHealth && a_value < 0.0F) {
					const auto [actorHandle, blockedDamageCount] =
						BlockHealthActorValueDamage(a_actorValueOwner);
					if (blockedDamageCount != 0) {
						LogBlockedHealthDamage(
							actorHandle,
							blockedDamageCount,
							"ModActorValue",
							-a_value);
						return;
					}
				}

				_original(a_actorValueOwner, a_modifier, a_actorValue, a_value);
			}

			static inline REL::Relocation<decltype(Thunk)> _original;
		};

		struct PuppetKillDyingHook
		{
			static void Thunk(RE::Actor* a_actor)
			{
				const auto [actorHandle, blockedDamageCount] =
					BlockHealthDamage(a_actor);
				if (blockedDamageCount != 0) {
					LogBlockedHealthDamage(
						actorHandle,
						blockedDamageCount,
						"KillDying",
						0.0F);
					return;
				}

				_original(a_actor);
			}

			static inline REL::Relocation<decltype(Thunk)> _original;
		};

		struct PuppetKillHook
		{
			static void Thunk(
				RE::Actor* a_actor,
				RE::Actor* a_attacker,
				float a_damage,
				bool a_sendEvent,
				bool a_ragdollInstant)
			{
				const auto [actorHandle, blockedDamageCount] =
					BlockHealthDamage(a_actor);
				if (blockedDamageCount != 0) {
					LogBlockedHealthDamage(
						actorHandle,
						blockedDamageCount,
						"KillImpl",
						a_damage);
					return;
				}

				_original(
					a_actor,
					a_attacker,
					a_damage,
					a_sendEvent,
					a_ragdollInstant);
			}

			static inline REL::Relocation<decltype(Thunk)> _original;
		};

		struct PuppetAnimationGraphHook
		{
			static bool Thunk(
				RE::IAnimationGraphManagerHolder* a_holder,
				const RE::BSFixedString& a_eventName)
			{
				std::uint32_t actorHandle = 0;
				std::uint32_t blockedEventCount = 0;
				bool firewallEnabled = false;
				bool allowClientManagedEvent = false;
				bool allowFurnitureEvent = false;
				bool isSwimStopEvent = false;
				const auto* eventName = a_eventName.c_str();
				{
					std::scoped_lock lock{ s_puppetGraphMutex };
					const auto puppet = s_puppetGraphs.find(a_holder);
					if (puppet != s_puppetGraphs.end()) {
						firewallEnabled = puppet->second.animationFirewallEnabled;
						actorHandle = puppet->second.actorHandle;
						const std::string_view eventView = eventName ?
							std::string_view{ eventName } : std::string_view{};
						// Swimming transitions are client-managed and do not interrupt
						// puppet animation. Weapon draw/sheathe requests can also reach
						// this hook from an animation worker after the authoritative request
						// was queued. Do not firewall either presentation-only event family;
						// combat AI, targeting, attacks, damage, and magic remain suppressed
						// elsewhere.
						allowClientManagedEvent =
							IsClientManagedAnimationEvent(eventView);
						allowFurnitureEvent =
							puppet->second.furnitureAnimationOwned;
						isSwimStopEvent = eventView == "swimStop";
					}
				}

				// Graph construction and the do-nothing package are allowed to finish
				// before ownership becomes exclusive. Filtering these setup events can
				// leave the newly spawned actor in its default pose. Native furniture
				// temporarily owns the graph while the puppet uses furniture; its event
				// names vary by furniture record and are generated locally by Skyrim.
				if (!firewallEnabled || allowClientManagedEvent ||
					allowFurnitureEvent) {
					const bool accepted = _original(a_holder, a_eventName);
					if (firewallEnabled && isSwimStopEvent && accepted) {
						std::scoped_lock lock{ s_puppetGraphMutex };
						const auto puppet = s_puppetGraphs.find(a_holder);
						if (puppet != s_puppetGraphs.end()) {
							++puppet->second.pendingSwimStops;
						}
					}
					return accepted;
				}

				{
					std::scoped_lock lock{ s_puppetGraphMutex };
					const auto puppet = s_puppetGraphs.find(a_holder);
					if (puppet != s_puppetGraphs.end()) {
						auto& count = puppet->second.blockedEventCounts[
							eventName ? eventName : "<null>"];
						blockedEventCount = ++count;
						if (eventName &&
							std::string_view{ eventName } == "IdleCowering") {
							++puppet->second.pendingCombatSanitizations;
						}
					}
				}

				if (blockedEventCount != 0 &&
					(blockedEventCount <= kInitialBlockedEventLogs ||
						blockedEventCount % kBlockedEventLogInterval == 0)) {
					logs::info(
						"Blocked local puppet animation event: actor={:08X} event={} count={}",
						actorHandle,
						a_eventName.c_str(),
						blockedEventCount);
				}

				return false;
			}

			static inline REL::Relocation<decltype(Thunk)> _original;
		};


		// Skyrim copies Actor-derived values into graph variables through channels
		// polled during each animation update, after main-loop graph writes. The
		// Direction channel reads the puppet's own movement request, which is always
		// forward, so replace its polled value with the network direction.
		struct PuppetDirectionChannelHook
		{
			// BSTAnimationGraphDataChannel<Actor, float, ActorDirectionChannel>:
			// BSAnimationGraphChannel::value at 0x18, then the channel's cached value
			// at 0x24 and its Actor at 0x28 (see RE::ActorSpeedChannel).
			static constexpr std::ptrdiff_t kValueOffset = 0x18;
			static constexpr std::ptrdiff_t kChannelValueOffset = 0x24;
			static constexpr std::ptrdiff_t kActorOffset = 0x28;

			static void Thunk(RE::BSAnimationGraphChannel* a_channel, bool a_arg)
			{
				_original(a_channel, a_arg);
				if (!a_channel) {
					return;
				}

				const auto* bytes = reinterpret_cast<std::byte*>(a_channel);
				const RE::Actor* actor = nullptr;
				std::memcpy(&actor, bytes + kActorOffset, sizeof(actor));
				// The pointer is only compared with registered puppets, never
				// dereferenced, so non-puppet channels are left untouched.
				const auto direction = GetAuthoritativeDirection(actor);
				if (!direction) {
					return;
				}

				auto* writableBytes = reinterpret_cast<std::byte*>(a_channel);
				std::memcpy(writableBytes + kValueOffset, &*direction, sizeof(float));
				std::memcpy(
					writableBytes + kChannelValueOffset,
					&*direction,
					sizeof(float));
			}

			static inline REL::Relocation<decltype(Thunk)> _original;
		};

		bool NotifyNetworkAnimationGraph(
			RE::Actor& a_actor,
			const RE::BSFixedString& a_eventName)
		{
			// Network-owned animation is the only source permitted to bypass the puppet
			// firewall. Calling the saved vfunc directly avoids classifying the event
			// at the public virtual dispatch point, where local game/mod events arrive.
			auto* holder = static_cast<RE::IAnimationGraphManagerHolder*>(
				std::addressof(a_actor));
			return PuppetAnimationGraphHook::_original(holder, a_eventName);
		}

		bool RevertAnimationGraphManager(RE::Actor& a_actor)
		{
			// CommonLibSSE-NG does not currently expose this wrapper. Skyrim Together
			// uses the same engine function before authoritative action replay. Its
			// Address Library IDs sit immediately before the graph-variable accessors:
			// Revert=32140/32883, SetBool=32141/32885.
			using func_t = bool(RE::IAnimationGraphManagerHolder*);
			static REL::Relocation<func_t> func{
				RELOCATION_ID(32140, 32883)
			};
			auto* holder = static_cast<RE::IAnimationGraphManagerHolder*>(
				std::addressof(a_actor));
			return func(holder);
		}

		void InvalidateAnimationDispatch(AnimationState& a_state)
		{
			a_state.hasDispatched = false;
			a_state.hasAttempted = false;
			a_state.hasVariableUpdateResult = false;
			a_state.lastVariableUpdateAccepted = false;
			a_state.hasCrossBlendedDirection = false;
			a_state.directionCrossBlendPending = false;
			a_state.failedDispatchAttempts = 0;
			a_state.rejectedEventAttempts = 0;
			a_state.rejectedEventElapsed = 0.0F;
			a_state.landingCompletionPending = false;
			a_state.landingCompletionWaitFrames = 0;
			a_state.landingCompletionWaitElapsed = 0.0F;
			a_state.weaponLocomotionReassertPending = false;
			a_state.weaponTransitionCompletionObserved = false;
			a_state.weaponTransitionWaitElapsed = 0.0F;
			a_state.weaponTransitionAnimationObserved = false;
			a_state.weaponTransitionExitForced = false;
			a_state.weaponDefaultStateDispatched = false;
			a_state.weaponReplayBaseDispatched = false;
			a_state.weaponTransitionMoveStartDispatched = false;
			a_state.weaponReplayDiagnosticFramesRemaining = 0;
			a_state.weaponRestoredWithoutCompletion = false;
			a_state.attackPlaybackPending = false;
			a_state.attackPlaybackObserved = false;
			a_state.attackPlaybackElapsed = 0.0F;
			a_state.attackLocomotionReassertPending = false;
			a_state.processedRevision.store(0, std::memory_order_release);
			a_state.graphManager = nullptr;
			a_state.landingEventSinkGraph = nullptr;
		}

		void AdvanceAttackPlayback(
			RE::Actor& a_actor,
			AnimationState& a_state,
			float a_deltaTime)
		{
			if (!a_state.attackPlaybackPending) {
				return;
			}

			a_state.attackPlaybackElapsed += std::clamp(
				a_deltaTime,
				0.0F,
				kMaximumFrameTime);
			const auto* actorState = a_actor.AsActorState();
			const bool attacking = actorState &&
				actorState->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone;
			a_state.attackPlaybackObserved =
				a_state.attackPlaybackObserved || attacking;
			const bool completed = a_state.attackPlaybackObserved && !attacking;
			const bool startTimedOut = !a_state.attackPlaybackObserved &&
				a_state.attackPlaybackElapsed >= kAttackStartFallbackSeconds;
			const bool playbackTimedOut = a_state.attackPlaybackElapsed >=
				kAttackPlaybackFallbackSeconds;
			if (!completed && !startTimedOut && !playbackTimedOut) {
				return;
			}

			logs::info(
				"Animation sync completed normal attack ownership: actor={:08X} observedPlayback={} elapsed={:.3f} startTimedOut={} playbackTimedOut={}",
				a_actor.GetHandle().native_handle(),
				a_state.attackPlaybackObserved,
				a_state.attackPlaybackElapsed,
				startTimedOut,
				playbackTimedOut);
			a_state.attackPlaybackPending = false;
			a_state.attackPlaybackObserved = false;
			a_state.attackPlaybackElapsed = 0.0F;
			// Native attacks normally return to locomotion themselves. Also request an
			// explicit replay without invalidating the drawn stance; treating this as a
			// full graph reset would incorrectly start another equip lifecycle.
			a_state.attackLocomotionReassertPending = true;
		}

		bool IsActorInHitReaction(RE::Actor& a_actor)
		{
			static const RE::BSFixedString isRecoilingVariable{ "IsRecoiling" };
			bool isRecoiling = false;
			const bool recoilAvailable = a_actor.GetGraphVariableBool(
				isRecoilingVariable,
				isRecoiling);
			const auto* actorState = a_actor.AsActorState();
			return a_actor.IsStaggering() || (recoilAvailable && isRecoiling) ||
				a_actor.IsInRagdollState() ||
				(actorState &&
					actorState->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal) ||
				a_actor.GetActorRuntimeData().boolBits.all(
					RE::Actor::BOOL_BITS::kParalyzed);
		}

		void ApplySmoothedHeading(
			RE::Actor& a_actor,
			float a_targetHeading,
			float a_deltaTime)
		{
			if (!std::isfinite(a_targetHeading) || a_deltaTime <= 0.0F) {
				return;
			}

			const float currentHeading = a_actor.GetAngleZ();
			if (!std::isfinite(currentHeading)) {
				a_actor.SetHeading(a_targetHeading);
				return;
			}

			// remainder selects the shortest route across the -pi/pi boundary. Limiting
			// that route keeps individual network samples from snapping the actor and
			// repeatedly provoking Skyrim's native NPC_Turn transitions.
			const float headingDelta = std::remainder(
				a_targetHeading - currentHeading,
				kTwoPi);
			if (std::abs(headingDelta) <= kHeadingDeadZone) {
				return;
			}

			const float maximumStep = kMaximumHeadingRate * a_deltaTime;
			const float step = std::clamp(
				headingDelta,
				-maximumStep,
				maximumStep);
			a_actor.SetHeading(currentHeading + step);
		}

		void BeginAnimationGraphRecovery(
			RE::Actor& a_actor,
			ControlledNPC& a_npc,
			std::uint32_t a_observedHitEvents,
			bool a_detectedHitReaction)
		{
			const auto animationState = a_npc.animationState;
			if (!animationState) {
				return;
			}

			RE::BSAnimationGraphManager* graphBeforeRecovery = nullptr;
			{
				RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
				if (a_actor.GetAnimationGraphManager(graphManager) && graphManager) {
					graphBeforeRecovery = graphManager.get();
				}
			}

			// TESHitEvent can be delivered while animation work is running elsewhere.
			// The event sink only increments a protected counter. This function is
			// reached from the authoritative main-loop hook, so the engine-owned
			// graph is reverted at a safe lifecycle boundary instead of being patched
			// from the event callback.
			DisablePuppetAnimationFirewall(a_npc);
			animationState->localInterruptionLatched = true;
			animationState->graphRecoveryInProgress = true;
			animationState->graphBeforeRecovery = graphBeforeRecovery;
			animationState->recoveryWaitFrames = 0;
			animationState->recoveryWaitElapsed = 0.0F;
			animationState->recoveryStableFrames = 0;
			InvalidateAnimationDispatch(*animationState);
			a_npc.ready = false;
			a_npc.initializationWaitFrames = 0;

			// Hit processing can enter the behavior graph below the public event hook
			// and can leave variables, active transitions, and pose state inconsistent
			// with replicated locomotion. Revert the complete graph to its initial
			// state. If the engine cannot do that, rebuilding 3D is the heavier fallback
			// which also reconstructs the graph.
			auto& runtimeData = a_actor.GetActorRuntimeData();
			const auto* actorState = a_actor.AsActorState();
			const bool requiresPhysicalReset = a_actor.IsInRagdollState() ||
				(actorState &&
					actorState->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal) ||
				runtimeData.boolBits.all(RE::Actor::BOOL_BITS::kParalyzed);
			runtimeData.boolBits.reset(RE::Actor::BOOL_BITS::kParalyzed);
			if (requiresPhysicalReset) {
				a_actor.PotentiallyFixRagdollState();
			}
			if (auto* root = a_actor.Get3D()) {
				root->RemoveDecals();
			}
			const bool graphReverted = !requiresPhysicalReset && graphBeforeRecovery &&
				RevertAnimationGraphManager(a_actor);
			animationState->graphRecoveryUsed3DReset = !graphReverted;
			if (!graphReverted) {
				a_actor.DoReset3D(false);
			}
			const auto recoveryCount = ++animationState->recoveryCount;
			logs::info(
				"Started puppet animation-graph recovery: actor={:08X} desired={} detectedReaction={} observedHits={} graphReverted={} fallback3DReset={} previousGraph={} recovery={}",
				a_npc.handle.native_handle(),
				Multiplayer::GetMovementStateName(
					a_npc.targetAnimationState.movementState),
				a_detectedHitReaction,
				a_observedHitEvents,
				graphReverted,
				!graphReverted,
				static_cast<const void*>(graphBeforeRecovery),
				recoveryCount);
		}

		bool RecoverAuthoritativeAnimation(
			RE::Actor& a_actor,
			ControlledNPC& a_npc,
			float a_deltaTime)
		{
			const auto animationState = a_npc.animationState;
			if (!animationState) {
				return false;
			}

			const auto pendingHitObservations = ConsumeHitObservations(a_npc);
			const bool isInterrupted = a_actor.Is3DLoaded() &&
				IsActorInHitReaction(a_actor);

			if (pendingHitObservations != 0) {
				animationState->observedHitEvents += pendingHitObservations;
				animationState->hitReactionObservationRemaining =
					kHitReactionObservationSeconds;
			}

			// TESHitEvent is emitted even when the damage firewall successfully makes
			// the hit harmless. Rebuild only after the graph or actor state confirms a
			// real reaction; the short observation window catches reactions that begin
			// a frame or two after their event.
			if (isInterrupted && !animationState->localInterruptionLatched) {
				const auto observedHitEvents = std::exchange(
					animationState->observedHitEvents,
					0);
				animationState->hitReactionObservationRemaining = 0.0F;
				BeginAnimationGraphRecovery(
					a_actor,
					a_npc,
					observedHitEvents,
					isInterrupted);
				return true;
			}

			const float deltaTime = std::clamp(a_deltaTime, 0.0F, kMaximumFrameTime);
			if (animationState->hitReactionObservationRemaining > 0.0F) {
				animationState->hitReactionObservationRemaining -= deltaTime;
				if (animationState->hitReactionObservationRemaining <= 0.0F) {
					animationState->hitReactionObservationRemaining = 0.0F;
					animationState->observedHitEvents = 0;
				}
			}

			if (!animationState->graphRecoveryInProgress) {
				if (!isInterrupted) {
					animationState->localInterruptionLatched = false;
				}
				return false;
			}

			++animationState->recoveryWaitFrames;
			animationState->recoveryWaitElapsed += deltaTime;
			RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
			const bool graphReady = a_actor.Is3DLoaded() &&
				a_actor.GetCharController() &&
				a_actor.GetAnimationGraphManager(graphManager) && graphManager;
			if (!graphReady) {
				animationState->recoveryStableFrames = 0;
				if (!animationState->graphRecoveryUsed3DReset &&
					animationState->recoveryWaitElapsed >=
						kGraphRecoveryFallbackSeconds) {
					animationState->graphRecoveryUsed3DReset = true;
					animationState->recoveryWaitFrames = 0;
					animationState->recoveryWaitElapsed = 0.0F;
					a_actor.DoReset3D(false);
					logs::warn(
						"Puppet graph revert did not become ready; escalating to 3D reset: actor={:08X} recovery={}",
						a_npc.handle.native_handle(),
						animationState->recoveryCount);
					return true;
				}
				if (animationState->recoveryWaitFrames == 1 ||
					animationState->recoveryWaitFrames %
						kGraphRecoveryWaitLogInterval == 0) {
					logs::info(
						"Waiting for recovered puppet animation graph: actor={:08X} loaded3D={} controller={} graph={} waitedFrames={} recovery={}",
						a_npc.handle.native_handle(),
						a_actor.Is3DLoaded(),
						a_actor.GetCharController() != nullptr,
						graphManager != nullptr,
						animationState->recoveryWaitFrames,
						animationState->recoveryCount);
				}
				return true;
			}

			++animationState->recoveryStableFrames;
			if (animationState->recoveryStableFrames < kGraphRecoveryStableFrames) {
				return true;
			}

			const bool graphWasReplaced =
				graphManager.get() != animationState->graphBeforeRecovery;
			const bool used3DReset = animationState->graphRecoveryUsed3DReset;
			animationState->graphRecoveryInProgress = false;
			animationState->graphRecoveryUsed3DReset = false;
			animationState->graphBeforeRecovery = nullptr;
			animationState->recoveryWaitFrames = 0;
			animationState->recoveryWaitElapsed = 0.0F;
			animationState->recoveryStableFrames = 0;
			// A hit event can arrive just before a shout transitions the actor into
			// ragdoll/paralysis. Allow the next frame to start physical recovery if
			// that state appeared after this recovery began.
			animationState->localInterruptionLatched = false;
			logs::info(
				"Recovered puppet animation graph is stable; replaying authoritative locomotion: actor={:08X} desired={} graphReplaced={} fallback3DReset={} recovery={}",
				a_npc.handle.native_handle(),
				Multiplayer::GetMovementStateName(
					a_npc.targetAnimationState.movementState),
				graphWasReplaced,
				used3DReset,
				animationState->recoveryCount);
			return false;
		}

		bool IsLocalReactionPackage(RE::PACKAGE_TYPE a_packageType)
		{
			switch (a_packageType) {
			case RE::PACKAGE_TYPE::kFleeNotCombat:
			case RE::PACKAGE_TYPE::kAlarm:
			case RE::PACKAGE_TYPE::kFlee:
			case RE::PACKAGE_TYPE::kSpectator:
			case RE::PACKAGE_TYPE::kSearchForAttacker:
			case RE::PACKAGE_TYPE::kAvoidPlayer:
				return true;
			default:
				return false;
			}
		}

		void SanitizeRemoteCombatState(RE::Actor& a_actor)
		{
			auto& runtimeData = a_actor.GetActorRuntimeData();
			const auto cowerRequests =
				ConsumeCombatSanitizationRequests(a_actor);

			const auto* currentPackage = a_actor.GetCurrentPackage();
			const auto packageType = currentPackage ?
				currentPackage->packData.packType.get() :
				RE::PACKAGE_TYPE::kNone;
			const bool reactionPackage = IsLocalReactionPackage(packageType);
			const bool wasInCombat = a_actor.IsInCombat();
			const bool hadCombatTarget = static_cast<bool>(
				runtimeData.currentCombatTarget);

			bool wasFleeing = false;
			if (const auto* middleHighProcess = a_actor.GetMiddleHighProcess()) {
				wasFleeing = middleHighProcess->isFleeing;
			}
			if (runtimeData.combatController &&
				runtimeData.combatController->state) {
				wasFleeing = wasFleeing ||
					runtimeData.combatController->state->isFleeing;
			}

			const bool needsAIReset = cowerRequests != 0 || reactionPackage ||
				wasInCombat || hadCombatTarget || wasFleeing;
			if (needsAIReset) {
				if (wasInCombat || hadCombatTarget || runtimeData.combatController) {
					a_actor.StopCombat();
				}
				runtimeData.currentCombatTarget.reset();
				if (auto* middleHighProcess = a_actor.GetMiddleHighProcess()) {
					middleHighProcess->isFleeing = false;
				}
				if (runtimeData.combatController &&
					runtimeData.combatController->state) {
					runtimeData.combatController->state->isFleeing = false;
				}
				if (reactionPackage || cowerRequests != 0) {
					a_actor.StopAlarmOnActor();
				}
				a_actor.InitiateDoNothingPackage();
				if (a_actor.GetEmotionType() == RE::EmotionType::kFear) {
					a_actor.SetEmotionType(RE::EmotionType::kNeutral);
				}
			}

			const auto* actorState = a_actor.AsActorState();
			const auto weaponState = actorState ? actorState->GetWeaponState() :
				RE::WEAPON_STATE::kSheathed;
			const bool authoritativeWeaponDrawn =
				GetAuthoritativeWeaponDrawnState(a_actor).value_or(false);
			const bool weaponTransitionPending = actorState &&
				(authoritativeWeaponDrawn ?
					weaponState != RE::WEAPON_STATE::kDrawn :
					weaponState != RE::WEAPON_STATE::kSheathed);
			std::uint32_t observationRequestCount = 0;
			if (cowerRequests != 0) {
				const auto [actorHandle, requestCount] = RequestHitObservation(&a_actor);
				(void) actorHandle;
				observationRequestCount = requestCount;
			}

			if (needsAIReset || weaponTransitionPending) {
				const auto [actorHandle, sanitizationCount] =
					RecordCombatSanitization(a_actor);
				if (cowerRequests != 0 ||
					sanitizationCount <= kInitialBlockedEventLogs ||
					sanitizationCount % kBlockedEventLogInterval == 0) {
					logs::info(
						"Sanitized local puppet combat presentation: actor={:08X} cowerEvents={} package={} combat={} target={} fleeing={} weaponState={} desiredWeaponDrawn={} sanitization={} observationRequest={}",
						actorHandle,
						cowerRequests,
						static_cast<std::underlying_type_t<RE::PACKAGE_TYPE>>(packageType),
						wasInCombat,
						hadCombatTarget,
						wasFleeing,
						static_cast<std::underlying_type_t<RE::WEAPON_STATE>>(weaponState),
						authoritativeWeaponDrawn,
						sanitizationCount,
						observationRequestCount);
				}
			}
		}

		void ApplyAuthoritativeWeaponPresentation(
			RE::Actor& a_actor,
			ControlledNPC& a_npc,
			bool a_moving,
			bool a_moveStartAlreadyDispatched)
		{
			const auto* actorState = a_actor.AsActorState();
			if (!actorState) {
				return;
			}

			const auto weaponState = actorState->GetWeaponState();
			const bool authoritativeWeaponDrawn =
				GetAuthoritativeWeaponDrawnState(a_actor).value_or(false);
			const bool weaponTransitionPending = authoritativeWeaponDrawn ?
				weaponState != RE::WEAPON_STATE::kDrawn :
				weaponState != RE::WEAPON_STATE::kSheathed;
			const bool transitionWasAnimating =
				IsWeaponTransitionAnimating(weaponState, authoritativeWeaponDrawn);
			// Queue (and retry) the native transition before selecting its moving
			// branch. Draw samples that edge as it enters the equip graph; waiting for
			// ActorState to report kDrawing costs a graph update and leaves the standing
			// equip clip visible while network translation continues. WantToDraw and
			// WantToSheathe only record an outstanding request, so keep retrying until
			// the matching transition is actually animating.
			if (weaponTransitionPending && !transitionWasAnimating) {
				a_actor.DrawWeaponMagicHands(authoritativeWeaponDrawn);
			}
			if (weaponTransitionPending && a_moving && a_npc.animationState &&
				!a_npc.animationState->weaponTransitionMoveStartDispatched) {
				static const RE::BSFixedString moveStartEvent{ "moveStart" };
				// Enter the equip graph through its normal standing state, then take its
				// explicit moveStart edge. Forcing iSyncIdleLocomotion before entry makes
				// the graph emit transition annotations while masking the visible pose.
				// A moveStart dispatched earlier in this update only counts if the
				// transition was already active; otherwise it preceded graph entry.
				const bool moveStartAccepted =
					(transitionWasAnimating && a_moveStartAlreadyDispatched) ||
					SendNetworkAnimationEvent(a_npc, moveStartEvent);
				if (moveStartAccepted) {
					a_npc.animationState->weaponTransitionMoveStartDispatched = true;
					logs::info(
						"Animation sync selected moving weapon-transition branch: actor={:08X} weaponDrawn={}",
						a_npc.handle.native_handle(),
						authoritativeWeaponDrawn);
				}
			}
		}

		void EnforceRemotePuppetSurvival(RE::Actor& a_actor)
		{
			auto& runtimeData = a_actor.GetActorRuntimeData();
			runtimeData.boolFlags.set(
				RE::Actor::BOOL_FLAGS::kEssential,
				RE::Actor::BOOL_FLAGS::kProtected);

			const auto* actorState = a_actor.AsActorState();
			const auto previousLifeState = actorState ?
				actorState->GetLifeState() :
				RE::ACTOR_LIFE_STATE::kAlive;
			bool recoveredLifeState = false;
			switch (previousLifeState) {
			case RE::ACTOR_LIFE_STATE::kDying:
			case RE::ACTOR_LIFE_STATE::kDead:
			case RE::ACTOR_LIFE_STATE::kRecycle:
				a_actor.Resurrect(false, a_actor.Is3DLoaded());
				recoveredLifeState = true;
				break;
			case RE::ACTOR_LIFE_STATE::kEssentialDown:
			case RE::ACTOR_LIFE_STATE::kBleedout:
				a_actor.SetLifeState(RE::ACTOR_LIFE_STATE::kAlive);
				a_actor.PotentiallyFixRagdollState();
				recoveredLifeState = true;
				break;
			default:
				break;
			}

			auto* actorValueOwner = a_actor.AsActorValueOwner();
			const float currentHealth = actorValueOwner ?
				actorValueOwner->GetActorValue(RE::ActorValue::kHealth) :
				0.0F;
			const float maximumHealth = a_actor.GetActorValueMax(
				RE::ActorValue::kHealth);
			const float missingHealth = maximumHealth - currentHealth;
			const bool restoredHealth = actorValueOwner &&
				std::isfinite(missingHealth) && missingHealth > 0.0F;
			if (restoredHealth) {
				actorValueOwner->RestoreActorValue(
					RE::ActorValue::kHealth,
					missingHealth);
			}

			if (recoveredLifeState || restoredHealth) {
				const auto [actorHandle, interventionCount] =
					BlockHealthDamage(&a_actor);
				std::uint32_t observationRequestCount = 0;
				if (recoveredLifeState) {
					const auto [observationActorHandle, requestCount] =
						RequestHitObservation(&a_actor);
					(void) observationActorHandle;
					observationRequestCount = requestCount;
				}
				if (interventionCount <= kInitialBlockedEventLogs ||
					interventionCount % kBlockedEventLogInterval == 0) {
					logs::info(
						"Reasserted remote-puppet survival state: actor={:08X} previousLifeState={} health={:.3f}/{:.3f} resurrected={} observationRequest={}",
						actorHandle,
						static_cast<std::underlying_type_t<RE::ACTOR_LIFE_STATE>>(
							previousLifeState),
						currentHealth,
						maximumHealth,
						recoveredLifeState,
						observationRequestCount);
				}
			}
		}

		void ApplyRemoteReplicaPolicy(RE::Actor& a_actor)
		{
			// These reference-local flags reduce normal friendly-hit presentation.
			// Survival is also reasserted because some concrete engine and console
			// routines bypass virtual damage hooks entirely.
			a_actor.formFlags |=
				RE::TESObjectREFR::RecordFlags::kIgnoreFriendlyHits;
			a_actor.GetActorRuntimeData().boolBits.set(
				RE::Actor::BOOL_BITS::kPlayerTeammate);
			EnforceRemotePuppetSurvival(a_actor);
			const auto dispelledEffects = DispelSuppressedMagicEffects(a_actor);
			if (dispelledEffects != 0) {
				const auto [actorHandle, requestCount] =
					RequestHitObservation(&a_actor);
				logs::info(
					"Removed suppressed magic from remote puppet; watching for an animation reaction: actor={:08X} effects={} request={}",
					actorHandle,
					dispelledEffects,
					requestCount);
			}

			// Preserve normal actor, graph, and controller processing. Only combat
			// decisions are suppressed; network movement still uses the live controller.
			auto& runtimeData = a_actor.GetActorRuntimeData();
			auto& flags = runtimeData.boolFlags;
			flags.reset(
				RE::Actor::BOOL_FLAGS::kCanDoFavor,
				RE::Actor::BOOL_FLAGS::kCanSpeak,
				RE::Actor::BOOL_FLAGS::kAttackOnSight,
				RE::Actor::BOOL_FLAGS::kAngryWithPlayer);
			flags.set(
				RE::Actor::BOOL_FLAGS::kAttackingDisabled,
				RE::Actor::BOOL_FLAGS::kCastingDisabled,
				RE::Actor::BOOL_FLAGS::kDoNotShowOnStealthMeter);

			// A do-nothing package does not itself opt out of combat. In particular,
			// CombatBehaviorDodgeThreat can still run for an unaggressive actor when a
			// nearby attack is detected. Disable that live process/controller path while
			// leaving ordinary actor and animation updates enabled.
			if (runtimeData.currentProcess) {
				runtimeData.currentProcess->ignoringCombat = true;
			}
			if (auto* middleHighProcess = a_actor.GetMiddleHighProcess()) {
				middleHighProcess->preventCombat = true;
			}
			if (runtimeData.combatController) {
				runtimeData.combatController->ignoringCombat = true;
				runtimeData.combatController->inactive = true;
			}
		}

		struct RemoteActorUpdateHook
		{
			static void Thunk(RE::Actor* a_actor, float a_delta)
			{
				// Reassert policy and persistent jump state immediately around Skyrim's
				// actor update. The local grounded controller must not overwrite the
				// network-owned bInJumpState between main-loop synchronization passes.
				std::optional<bool> authoritativeJumpState;
				if (IsAuthoritativeGameThread()) {
					authoritativeJumpState = GetAuthoritativeJumpState(a_actor);
				}
				const bool protectRemotePuppet =
					authoritativeJumpState.has_value();
				if (protectRemotePuppet) {
					ApplyRemoteReplicaPolicy(*a_actor);
					(void) ApplyAuthoritativeJumpState(
						*a_actor,
						*authoritativeJumpState);
				}
				_original(a_actor, a_delta);
				if (protectRemotePuppet) {
					(void) ApplyAuthoritativeJumpState(
						*a_actor,
						*authoritativeJumpState);
					EnforceRemotePuppetSurvival(*a_actor);
				}
			}

			static inline REL::Relocation<decltype(Thunk)> _original;
		};

		bool IsMoving(Multiplayer::MovementState a_state)
		{
			return a_state != Multiplayer::MovementState::kIdle &&
				a_state != Multiplayer::MovementState::kSneakingIdle;
		}

		bool IsSneaking(Multiplayer::MovementState a_state)
		{
			return a_state == Multiplayer::MovementState::kSneakingIdle ||
				a_state == Multiplayer::MovementState::kSneakingWalking ||
				a_state == Multiplayer::MovementState::kSneakingRunning;
		}

		bool IsSprinting(Multiplayer::MovementState a_state)
		{
			return a_state == Multiplayer::MovementState::kSprinting;
		}

		float GetGraphSpeedForState(
			RE::Actor& a_actor,
			float a_sampleSpeed,
			Multiplayer::MovementState a_state)
		{
			if (a_state == Multiplayer::MovementState::kIdle ||
				a_state == Multiplayer::MovementState::kSneakingIdle) {
				return 0.0F;
			}

			// Speed and locomotion state must describe the same network sample. The
			// actor's base speed is only a fallback for a zero/invalid first sample.
			if (std::isfinite(a_sampleSpeed) && a_sampleSpeed > 0.0F) {
				return a_sampleSpeed;
			}

			float graphSpeed = 0.0F;
			switch (a_state) {
			case Multiplayer::MovementState::kWalking:
			case Multiplayer::MovementState::kSneakingWalking:
				graphSpeed = a_actor.GetWalkSpeed();
				break;
			case Multiplayer::MovementState::kRunning:
			case Multiplayer::MovementState::kSprinting:
			case Multiplayer::MovementState::kSneakingRunning:
				graphSpeed = a_actor.GetRunSpeed();
				break;
			case Multiplayer::MovementState::kIdle:
			case Multiplayer::MovementState::kSneakingIdle:
				break;
			}

			return std::isfinite(graphSpeed) && graphSpeed > 0.0F ?
				graphSpeed : 1.0F;
		}

		void RequestAnimationUpdate(
			const std::shared_ptr<AnimationState>& a_animationState,
			Multiplayer::AnimationState a_state,
			float a_speed,
			float a_direction)
		{
			if (!a_animationState) {
				return;
			}

			a_animationState->desired.store(
				a_state.movementState,
				std::memory_order_relaxed);
			a_animationState->desiredJumping.store(
				a_state.jumping,
				std::memory_order_relaxed);
			a_animationState->desiredWeaponDrawn.store(
				a_state.weaponDrawn,
				std::memory_order_relaxed);
			a_animationState->desiredSpeed.store(a_speed, std::memory_order_relaxed);
			a_animationState->desiredDirection.store(
				a_direction,
				std::memory_order_relaxed);
			a_animationState->desiredRevision.fetch_add(
				1,
				std::memory_order_release);
		}

		void ApplyAnimationUpdate(
			RE::Actor& a_actor,
			ControlledNPC& a_npc,
			float a_deltaTime)
		{
			const auto animationState = a_npc.animationState;
			if (!animationState) {
				return;
			}

			static const RE::BSFixedString moveStartEvent{ "moveStart" };
			static const RE::BSFixedString moveStopEvent{ "moveStop" };
			static const RE::BSFixedString sneakStartEvent{ "sneakStart" };
			static const RE::BSFixedString sneakStopEvent{ "sneakStop" };
			static const RE::BSFixedString sprintStartEvent{ "SprintStart" };
			static const RE::BSFixedString sprintStopEvent{ "SprintStop" };
			static const RE::BSFixedString jumpStandingStartEvent{
				"JumpStandingStart"
			};
			static const RE::BSFixedString jumpDirectionalStartEvent{
				"JumpDirectionalStart"
			};
			static const RE::BSFixedString jumpLandEvent{ "JumpLand" };
			static const RE::BSFixedString cyclicFreezeEvent{ "CyclicFreeze" };
			static const RE::BSFixedString cyclicCrossBlendEvent{
				"CyclicCrossBlend"
			};
			static const RE::BSFixedString jumpLandEndEvent{ "JumpLandEnd" };
			static const RE::BSFixedString weaponEquipOutEvent{
				"WeapEquip_Out"
			};
			static const RE::BSFixedString weaponEquipOutMovingEvent{
				"WeapEquip_OutMoving"
			};
			static const RE::BSFixedString weaponUnequipOutEvent{ "Unequip_Out" };
			static const RE::BSFixedString weaponUnequipOutMovingEvent{
				"Unequip_OutMoving"
			};
			static const RE::BSFixedString idleForceDefaultStateEvent{
				"IdleForceDefaultState"
			};
			static const RE::BSFixedString inJumpStateVariable{ "bInJumpState" };
			static const RE::BSFixedString speedDampedVariable{ "SpeedDamped" };
			static const RE::BSFixedString syncIdleLocomotionVariable{
				"iSyncIdleLocomotion"
			};
			static const RE::BSFixedString syncForwardStateVariable{
				"iSyncForwardState"
			};
			static const RE::BSFixedString syncSprintStateVariable{
				"iSyncSprintState"
			};
			static const RE::BSFixedString animationDrivenVariable{
				"bAnimationDriven"
			};
			static const RE::BSFixedString equipOKVariable{ "bEquipOk" };

			// The caller is already running in the authoritative main-loop hook.
			// Apply graph state before direct translation so a single network sample
			// cannot advance the transform with stale locomotion variables.
			const auto actorHandle = a_actor.GetHandle();
			if (!a_actor.Is3DLoaded()) {
				DisablePuppetAnimationFirewall(a_npc);
				SetLandingCompletionObservation(a_actor, false);
				if (animationState->hasDispatched || animationState->graphManager) {
					logs::info(
						"Animation sync reset graph state: actor={:08X} reason=actor-or-3d-unavailable",
						actorHandle.native_handle());
				}
				animationState->hasDispatched = false;
				animationState->hasAttempted = false;
				animationState->hasVariableUpdateResult = false;
				animationState->failedDispatchAttempts = 0;
				animationState->rejectedEventAttempts = 0;
				animationState->rejectedEventElapsed = 0.0F;
				animationState->landingCompletionPending = false;
				animationState->landingCompletionWaitFrames = 0;
				animationState->landingCompletionWaitElapsed = 0.0F;
				animationState->weaponLocomotionReassertPending = false;
				animationState->weaponTransitionCompletionObserved = false;
				animationState->weaponTransitionWaitElapsed = 0.0F;
				animationState->weaponTransitionAnimationObserved = false;
				animationState->weaponTransitionExitForced = false;
				animationState->weaponDefaultStateDispatched = false;
				animationState->weaponReplayBaseDispatched = false;
				animationState->weaponTransitionMoveStartDispatched = false;
				animationState->attackPlaybackPending = false;
				animationState->attackPlaybackObserved = false;
				animationState->attackPlaybackElapsed = 0.0F;
				animationState->attackLocomotionReassertPending = false;
				animationState->processedRevision.store(
					0,
					std::memory_order_release);
				animationState->graphManager = nullptr;
				animationState->landingEventSinkGraph = nullptr;
				return;
			}

			auto* actor = &a_actor;
			RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
			if (!actor->GetAnimationGraphManager(graphManager) || !graphManager) {
				DisablePuppetAnimationFirewall(a_npc);
				SetLandingCompletionObservation(*actor, false);
				if (animationState->hasDispatched || animationState->graphManager) {
					logs::info(
						"Animation sync reset graph state: actor={:08X} reason=graph-unavailable",
						actorHandle.native_handle());
				}
				animationState->hasDispatched = false;
				animationState->hasAttempted = false;
				animationState->hasVariableUpdateResult = false;
				animationState->failedDispatchAttempts = 0;
				animationState->rejectedEventAttempts = 0;
				animationState->rejectedEventElapsed = 0.0F;
				animationState->landingCompletionPending = false;
				animationState->landingCompletionWaitFrames = 0;
				animationState->landingCompletionWaitElapsed = 0.0F;
				animationState->weaponLocomotionReassertPending = false;
				animationState->weaponTransitionCompletionObserved = false;
				animationState->weaponTransitionWaitElapsed = 0.0F;
				animationState->weaponTransitionAnimationObserved = false;
				animationState->weaponTransitionExitForced = false;
				animationState->weaponReplayBaseDispatched = false;
				animationState->weaponTransitionMoveStartDispatched = false;
				animationState->attackPlaybackPending = false;
				animationState->attackPlaybackObserved = false;
				animationState->attackPlaybackElapsed = 0.0F;
				animationState->attackLocomotionReassertPending = false;
				animationState->processedRevision.store(
					0,
					std::memory_order_release);
				animationState->graphManager = nullptr;
				animationState->landingEventSinkGraph = nullptr;
				return;
			}
			if (animationState->graphManager != graphManager.get()) {
				DisablePuppetAnimationFirewall(a_npc);
				SetLandingCompletionObservation(*actor, false);
				logs::info(
					"Animation sync detected {} graph: actor={:08X}; next state will be fully asserted",
					animationState->graphManager ? "replacement" : "initial",
					actorHandle.native_handle());
				animationState->graphManager = graphManager.get();
				animationState->hasDispatched = false;
				animationState->hasAttempted = false;
				animationState->hasVariableUpdateResult = false;
				animationState->processedRevision.store(
					0,
					std::memory_order_release);
				animationState->failedDispatchAttempts = 0;
				animationState->rejectedEventAttempts = 0;
				animationState->rejectedEventElapsed = 0.0F;
				animationState->landingCompletionPending = false;
				animationState->landingCompletionWaitFrames = 0;
				animationState->landingCompletionWaitElapsed = 0.0F;
				animationState->weaponLocomotionReassertPending = false;
				animationState->weaponTransitionCompletionObserved = false;
				animationState->weaponTransitionWaitElapsed = 0.0F;
				animationState->weaponTransitionAnimationObserved = false;
				animationState->weaponTransitionExitForced = false;
				animationState->weaponDefaultStateDispatched = false;
				animationState->weaponReplayBaseDispatched = false;
				animationState->weaponTransitionMoveStartDispatched = false;
				animationState->weaponReplayDiagnosticFramesRemaining = 0;
				animationState->attackPlaybackPending = false;
				animationState->attackPlaybackObserved = false;
				animationState->attackPlaybackElapsed = 0.0F;
				animationState->attackLocomotionReassertPending = false;
			}
			bool landingEventSinkRegistered =
				animationState->landingEventSinkGraph == graphManager.get();
			if (!landingEventSinkRegistered) {
				landingEventSinkRegistered =
					RegisterLandingCompletionEventSink(*graphManager);
				if (landingEventSinkRegistered) {
					animationState->landingEventSinkGraph = graphManager.get();
					logs::info(
						"Registered puppet animation output-event sink: actor={:08X}",
						actorHandle.native_handle());
				}
			}

			if (animationState->weaponReplayDiagnosticFramesRemaining != 0) {
				constexpr std::uint32_t kDiagnosticFrameCount = 30;
				const auto diagnosticFrame = kDiagnosticFrameCount -
					animationState->weaponReplayDiagnosticFramesRemaining + 1;
				if (diagnosticFrame == 1 || diagnosticFrame == 2 ||
					diagnosticFrame == 4 || diagnosticFrame == 8 ||
					diagnosticFrame == 16 || diagnosticFrame == kDiagnosticFrameCount) {
					std::int32_t syncIdleLocomotion = 0;
					std::int32_t syncForwardState = 0;
					std::int32_t syncSprintState = 0;
					bool animationDriven = false;
					bool equipOK = false;
					const bool syncIdleAvailable = actor->GetGraphVariableInt(
						syncIdleLocomotionVariable,
						syncIdleLocomotion);
					const bool syncForwardAvailable = actor->GetGraphVariableInt(
						syncForwardStateVariable,
						syncForwardState);
					const bool syncSprintAvailable = actor->GetGraphVariableInt(
						syncSprintStateVariable,
						syncSprintState);
					const bool animationDrivenAvailable = actor->GetGraphVariableBool(
						animationDrivenVariable,
						animationDriven);
					const bool equipOKAvailable = actor->GetGraphVariableBool(
						equipOKVariable,
						equipOK);
					const auto* diagnosticActorState = actor->AsActorState();
					const auto diagnosticWeaponState = diagnosticActorState ?
						diagnosticActorState->GetWeaponState() :
						RE::WEAPON_STATE::kSheathed;
					logs::info(
						"Animation sync weapon replay timeline: actor={:08X} weaponDrawn={} frame={}/{} weaponState={} syncIdle={}/{} syncForward={}/{} syncSprint={}/{} animationDriven={}/{} equipOK={}/{}",
						actorHandle.native_handle(),
						animationState->weaponReplayDiagnosticWeaponDrawn,
						diagnosticFrame,
						kDiagnosticFrameCount,
						static_cast<std::underlying_type_t<RE::WEAPON_STATE>>(
							diagnosticWeaponState),
						syncIdleAvailable,
						syncIdleLocomotion,
						syncForwardAvailable,
						syncForwardState,
						syncSprintAvailable,
						syncSprintState,
						animationDrivenAvailable,
						animationDriven,
						equipOKAvailable,
						equipOK);
				}
				--animationState->weaponReplayDiagnosticFramesRemaining;
			}

			if (animationState->landingCompletionPending) {
				const auto completedLandings = ConsumeJumpLandEnds(a_npc);
				bool landingCompleted = completedLandings != 0;
				bool forcedCompletion = false;
				bool forcedEventAccepted = false;
				const auto landingWaitFrames =
					++animationState->landingCompletionWaitFrames;
				animationState->landingCompletionWaitElapsed += std::clamp(
					a_deltaTime,
					0.0F,
					kMaximumFrameTime);
				if (!landingCompleted &&
					(landingWaitFrames == 1 || landingWaitFrames % 15 == 0)) {
					const auto currentPosition = actor->GetPosition();
					bool graphJumping = false;
					const bool graphJumpAvailable = actor->GetGraphVariableBool(
						inJumpStateVariable,
						graphJumping);
					logs::info(
						"Animation sync landing timeline: actor={:08X} waitedFrames={} current=({:.3f},{:.3f},{:.3f}) target=({:.3f},{:.3f},{:.3f}) zError={:.3f} physicalJump={} physicalMidair={} graphJumpAvailable={} graphJump={}",
						actorHandle.native_handle(),
						landingWaitFrames,
						currentPosition.x,
						currentPosition.y,
						currentPosition.z,
						a_npc.targetPosition.x,
						a_npc.targetPosition.y,
						a_npc.targetPosition.z,
						a_npc.targetPosition.z - currentPosition.z,
						actor->IsInJumpState(),
						actor->IsInMidair(),
						graphJumpAvailable,
						graphJumping);
				}
				if (!landingCompleted &&
					animationState->landingCompletionWaitElapsed >=
						kLandingCompletionFallbackSeconds) {
					// A custom behavior graph may omit its normal annotation. Bound the
					// wait and drive the same lifecycle edge explicitly so the puppet
					// cannot remain in a landing branch indefinitely.
					forcedEventAccepted = SendNetworkAnimationEvent(
						a_npc,
						jumpLandEndEvent);
					landingCompleted = true;
					forcedCompletion = true;
				}

				if (landingCompleted) {
					const auto completedAfterFrames = landingWaitFrames;
					animationState->landingCompletionPending = false;
					animationState->landingCompletionWaitFrames = 0;
					animationState->landingCompletionWaitElapsed = 0.0F;
					SetLandingCompletionObservation(*actor, false);
					// JumpLandEnd selects the graph's exit branch. Reassert every active
					// grounded axis afterward—even if the network movement enum did not
					// change—so the actor cannot exit into idle and slide.
					animationState->hasDispatched = false;
					animationState->hasAttempted = false;
					animationState->hasVariableUpdateResult = false;
					animationState->failedDispatchAttempts = 0;
					animationState->rejectedEventAttempts = 0;
					animationState->rejectedEventElapsed = 0.0F;
					logs::info(
						"Animation sync completing landing branch and reasserting grounded locomotion: actor={:08X} observedMarkers={} waitedFrames={} forced={} forcedEventAccepted={}",
						actorHandle.native_handle(),
						completedLandings,
						completedAfterFrames,
						forcedCompletion,
						forcedEventAccepted);
				}
			} else {
				// Drain a duplicate graph annotation while no landing transition owns
				// it. The next takeoff also closes and clears this observation window.
				(void) ConsumeJumpLandEnds(a_npc);
			}

			const auto desiredRevision = animationState->desiredRevision.load(
				std::memory_order_acquire);
			const Multiplayer::AnimationState authoritativeState{
				.movementState = animationState->desired.load(
					std::memory_order_acquire),
				.jumping = animationState->desiredJumping.load(
					std::memory_order_acquire),
				.weaponDrawn = animationState->desiredWeaponDrawn.load(
					std::memory_order_acquire)
			};
			const auto sampleSpeed = animationState->desiredSpeed.load(
				std::memory_order_acquire);
			const auto sampleDirection = animationState->desiredDirection.load(
				std::memory_order_acquire);
			const bool authoritativeDispatchRequired =
				!animationState->hasDispatched ||
				animationState->lastDispatched != authoritativeState;
			const auto previousState = animationState->lastDispatched;
			const bool hadPreviouslyDispatched = animationState->hasDispatched;
			const bool weaponDrawnChanged = authoritativeDispatchRequired &&
				(hadPreviouslyDispatched ?
					previousState.weaponDrawn != authoritativeState.weaponDrawn :
					authoritativeState.weaponDrawn);
			if (weaponDrawnChanged) {
				animationState->weaponLocomotionReassertPending = true;
				animationState->weaponTransitionCompletionObserved = false;
				animationState->weaponTransitionWaitElapsed = 0.0F;
				animationState->weaponTransitionAnimationObserved = false;
				animationState->weaponTransitionExitForced = false;
				animationState->weaponDefaultStateDispatched = false;
				animationState->weaponReplayBaseDispatched = false;
				animationState->weaponTransitionMoveStartDispatched = false;
				animationState->weaponReplayDiagnosticFramesRemaining = 0;
				animationState->weaponRestoredWithoutCompletion = false;
			}
			const auto* actorState = actor->AsActorState();
			const auto weaponState = actorState ? actorState->GetWeaponState() :
				RE::WEAPON_STATE::kSheathed;
			const auto observedWeaponTransitionEnds =
				ConsumeWeaponTransitionEnds(a_npc);
			if (observedWeaponTransitionEnds != 0) {
				animationState->weaponTransitionCompletionObserved = true;
				const bool lateCompletion =
					!animationState->weaponLocomotionReassertPending &&
					animationState->weaponRestoredWithoutCompletion;
				if (lateCompletion) {
					// The bounded fallback restored locomotion while the clip was still
					// playing, so the clip consumed that replay and its exit leaves the
					// graph in a standing stance. Replay locomotion again from this exit.
					animationState->weaponRestoredWithoutCompletion = false;
					animationState->weaponLocomotionReassertPending = true;
					animationState->weaponTransitionWaitElapsed = 0.0F;
					animationState->weaponTransitionAnimationObserved = false;
					animationState->weaponTransitionExitForced = false;
					animationState->weaponDefaultStateDispatched = false;
					animationState->weaponReplayBaseDispatched = false;
					animationState->weaponTransitionMoveStartDispatched = false;
				}
				logs::info(
					"Animation sync observed weapon transition exit: actor={:08X} weaponDrawn={} weaponState={} markers={} lateCompletion={}",
					actorHandle.native_handle(),
					authoritativeState.weaponDrawn,
					static_cast<std::underlying_type_t<RE::WEAPON_STATE>>(
						weaponState),
					observedWeaponTransitionEnds,
					lateCompletion);
				if (authoritativeState.weaponDrawn) {
					// Resetting a successfully drawn graph to its default state makes its
					// behavior state sheathed while ActorState remains drawn. Preserve the
					// drawn branch and only allow its worker-thread exit one update to settle.
					return;
				}
			}
			if (animationState->weaponLocomotionReassertPending &&
				!animationState->weaponTransitionCompletionObserved &&
				!animationState->weaponTransitionExitForced &&
				!animationState->weaponTransitionAnimationObserved &&
				IsWeaponTransitionAnimating(
					weaponState,
					authoritativeState.weaponDrawn)) {
				// A request that flips during an opposite transition (for example a
				// draw started just before a sheathe arrives) spends most of the budget
				// on that clip. Timing out mid-sheathe resets the graph before its
				// weaponSheathe annotation, leaving the weapon in hand. Restart the
				// bounded wait once, when the requested clip actually starts.
				animationState->weaponTransitionAnimationObserved = true;
				animationState->weaponTransitionWaitElapsed = 0.0F;
			}
			if (animationState->weaponLocomotionReassertPending &&
				animationState->weaponTransitionWaitElapsed <
					kWeaponTransitionFallbackSeconds) {
				animationState->weaponTransitionWaitElapsed += std::clamp(
					a_deltaTime,
					0.0F,
					kMaximumFrameTime);
			}
			const bool weaponTransitionTimedOut =
				animationState->weaponTransitionWaitElapsed >=
					kWeaponTransitionFallbackSeconds;
			const bool weaponTransitionCompleted =
				animationState->weaponTransitionCompletionObserved ||
				weaponTransitionTimedOut;
			if (animationState->weaponLocomotionReassertPending &&
				weaponTransitionTimedOut &&
				!animationState->weaponTransitionCompletionObserved &&
				!animationState->weaponTransitionExitForced) {
				const bool authoritativeIsMoving =
					IsMoving(authoritativeState.movementState);
				const auto& transitionEndEvent = authoritativeState.weaponDrawn ?
					(authoritativeIsMoving ?
						weaponEquipOutMovingEvent : weaponEquipOutEvent) :
					(authoritativeIsMoving ?
						weaponUnequipOutMovingEvent : weaponUnequipOutEvent);
				const bool forcedExitAccepted = SendNetworkAnimationEvent(
					a_npc,
					transitionEndEvent);
				animationState->weaponTransitionExitForced = true;
				logs::warn(
					"Animation sync weapon transition completion timed out; sent bounded exit: actor={:08X} weaponDrawn={} weaponState={} waitedSeconds={:.3f} event={} accepted={}",
					actorHandle.native_handle(),
					authoritativeState.weaponDrawn,
					static_cast<std::underlying_type_t<RE::WEAPON_STATE>>(
						weaponState),
					animationState->weaponTransitionWaitElapsed,
					transitionEndEvent.c_str(),
					forcedExitAccepted);
				if (forcedExitAccepted) {
					// Give an accepted exit edge one graph update before restoring
					// locomotion. No longer-lived recovery gate is introduced.
					return;
				}
			}
			if (animationState->weaponLocomotionReassertPending &&
				weaponTransitionCompleted &&
				!authoritativeState.weaponDrawn &&
				!animationState->weaponDefaultStateDispatched) {
				// The unequip exit annotation finishes the clip, but does not guarantee
				// that the behavior graph is in a state that can consume locomotion. A
				// direct moveStart can report success while being ignored. Once ActorState
				// is also sheathed, force the graph's matching default state first and
				// replay authoritative locomotion on the next update.
				const bool defaultStateAccepted = SendNetworkAnimationEvent(
					a_npc,
					idleForceDefaultStateEvent);
				animationState->weaponDefaultStateDispatched = true;
				logs::info(
					"Animation sync reset graph to default after weapon transition: actor={:08X} weaponDrawn={} weaponState={} completionMarkerObserved={} eventAccepted={}",
					actorHandle.native_handle(),
					authoritativeState.weaponDrawn,
					static_cast<std::underlying_type_t<RE::WEAPON_STATE>>(
						weaponState),
					animationState->weaponTransitionCompletionObserved,
					defaultStateAccepted);
				return;
			}
			const auto observedSwimStops = ConsumeSwimStops(a_npc);
			if (observedSwimStops != 0) {
				animationState->swimLocomotionReassertPending = true;
			}
			const bool authoritativeLocomotionActive =
				IsMoving(authoritativeState.movementState) ||
				authoritativeState.jumping;
			const bool reassertLocomotionAfterWeaponTransition =
				ShouldReassertAfterWeaponTransition(
					animationState->weaponLocomotionReassertPending,
					weaponTransitionCompleted,
					IsMoving(authoritativeState.movementState),
					authoritativeState.jumping);
			const bool reassertLocomotionAfterSwimStop =
				animationState->swimLocomotionReassertPending && actorState &&
				!actorState->IsSwimming();
			const bool reassertLocomotionAfterAttack =
				animationState->attackLocomotionReassertPending &&
				!animationState->attackPlaybackPending;
			// A weapon branch may consume locomotion events. Replay a valid base edge
			// first, then layer sprint/sneak movement/jump on a later graph update.
			// Sending every edge together can make each one report success while no
			// runnable locomotion branch actually becomes active.
			const auto replayBaseState =
				GetWeaponReplayBaseState(authoritativeState);
			if (animationState->weaponLocomotionReassertPending &&
				animationState->weaponReplayBaseDispatched &&
				!authoritativeLocomotionActive) {
				// Do not carry a partially replayed base across an idle interval.
				animationState->weaponReplayBaseDispatched = false;
			}
			const bool replayingWeaponBase =
				reassertLocomotionAfterWeaponTransition &&
				!animationState->weaponReplayBaseDispatched;
			Multiplayer::AnimationState state = authoritativeState;
			if (replayingWeaponBase) {
				state = replayBaseState;
			} else if (reassertLocomotionAfterWeaponTransition &&
				authoritativeState.jumping) {
				auto groundedState = authoritativeState;
				groundedState.jumping = false;
				if (animationState->lastDispatched != groundedState) {
					// Sprint/sneak movement is another graph edge. Establish it before
					// dispatching takeoff rather than stacking both on one update.
					state = groundedState;
				}
			}
			const bool weaponReplayNeedsFollowup =
				reassertLocomotionAfterWeaponTransition &&
				state != authoritativeState;
			const bool isMoving = IsMoving(state.movementState);
			const bool isSneaking = IsSneaking(state.movementState);
			const bool isSprinting = IsSprinting(state.movementState);
			const float graphSpeed = GetGraphSpeedForState(
				*actor,
				sampleSpeed,
				state.movementState);
			const bool hadPreviousState = hadPreviouslyDispatched &&
				!replayingWeaponBase;
			const bool dispatchRequired =
				reassertLocomotionAfterWeaponTransition ||
				!animationState->hasDispatched ||
				animationState->lastDispatched != state;
			const bool jumpRequired = dispatchRequired && (hadPreviousState ?
				previousState.jumping != state.jumping : state.jumping);
			const bool jumpLandRequired = jumpRequired && !state.jumping;
			if (jumpLandRequired && !landingEventSinkRegistered) {
				// Do not enter a landing branch whose completion marker cannot yet be
				// observed. Graph construction can expose the manager one pass before
				// its first animation graph becomes available.
				return;
			}
			// The landing checkpoint's variables must represent current movement,
			// not takeoff movement. Ordinary locomotion events are deferred until
			// JumpLandEnd, when the full grounded state is reasserted.
			const bool moveRequired = !jumpLandRequired &&
				((dispatchRequired && (hadPreviousState ?
					IsMoving(previousState.movementState) != isMoving : isMoving)) ||
					((replayingWeaponBase ||
						reassertLocomotionAfterSwimStop ||
						reassertLocomotionAfterAttack) && isMoving));
			const bool sneakRequired = !jumpLandRequired &&
				((dispatchRequired && (hadPreviousState ?
					IsSneaking(previousState.movementState) != isSneaking : isSneaking)) ||
					((replayingWeaponBase ||
						reassertLocomotionAfterSwimStop ||
						reassertLocomotionAfterAttack) && isSneaking));
			const bool sprintRequired = !jumpLandRequired &&
				((dispatchRequired && (hadPreviousState ?
					IsSprinting(previousState.movementState) != isSprinting : isSprinting)) ||
					((replayingWeaponBase ||
						reassertLocomotionAfterSwimStop ||
						reassertLocomotionAfterAttack) && isSprinting));
			const bool eventDispatchRequired = dispatchRequired ||
				reassertLocomotionAfterSwimStop ||
				reassertLocomotionAfterAttack;
			if (dispatchRequired && (!animationState->hasAttempted ||
				animationState->lastAttempted != state)) {
				animationState->lastAttempted = state;
				animationState->hasAttempted = true;
				animationState->failedDispatchAttempts = 0;
				animationState->rejectedEventAttempts = 0;
				animationState->rejectedEventElapsed = 0.0F;
			}

			auto* strings = RE::FixedStrings::GetSingleton();
			const bool stringsAvailable = strings != nullptr;
			// Stop edges are evaluated against the old graph variables. Clearing
			// Speed or IsSprinting first made the recorded sprint/idle exits reject
			// deterministically and left the graph in its previous branch.
			const bool moveStopRequired = moveRequired && !isMoving;
			const bool sneakStopRequired = sneakRequired && !isSneaking;
			const bool sprintStopRequired = sprintRequired && !isSprinting;
			if (jumpRequired) {
				// JumpLand starts a timed landing branch whose graph output emits
				// JumpLandEnd later, often from an animation worker thread. Observe that
				// output only while the network-authoritative state is grounded.
				SetLandingCompletionObservation(*actor, !state.jumping);
			}
			bool moveAccepted = moveStopRequired && stringsAvailable ?
				SendNetworkAnimationEvent(a_npc, moveStopEvent) : false;
			bool sneakAccepted = sneakStopRequired && stringsAvailable ?
				SendNetworkAnimationEvent(a_npc, sneakStopEvent) : false;
			bool sprintAccepted = sprintStopRequired && stringsAvailable ?
				SendNetworkAnimationEvent(a_npc, sprintStopEvent) : false;
			bool jumpAccepted = false;
			const bool stopEventsAccepted =
				(!moveStopRequired || moveAccepted) &&
				(!sneakStopRequired || sneakAccepted) &&
				(!sprintStopRequired || sprintAccepted);
			const auto stopEventAttempt =
				animationState->rejectedEventAttempts + 1;
			if (!stopEventsAccepted &&
				(stopEventAttempt < kMaximumEventDispatchAttempts ||
					animationState->rejectedEventElapsed < kMinimumEventRetrySeconds)) {
				logs::info(
					"Animation sync graph dispatch: actor={:08X} revision={} previous={}/{} desired={}/{} result=retrying-stop-events strings={} moveEvent={}/{} sneakEvent={}/{} sprintEvent={}/{} jumpEvent=deferred/{} eventAttempt={}/{}",
					actorHandle.native_handle(),
					static_cast<unsigned long long>(desiredRevision),
					hadPreviousState ?
						Multiplayer::GetMovementStateName(
							previousState.movementState) :
						"uninitialized",
					hadPreviousState && previousState.jumping ?
						"jumping" : "grounded",
					Multiplayer::GetMovementStateName(state.movementState),
					state.jumping ? "jumping" : "grounded",
					stringsAvailable,
					moveAccepted,
					moveStopRequired,
					sneakAccepted,
					sneakStopRequired,
					sprintAccepted,
					sprintStopRequired,
					jumpLandRequired,
					stopEventAttempt,
					kMaximumEventDispatchAttempts);
				++animationState->rejectedEventAttempts;
				animationState->rejectedEventElapsed += std::clamp(
					a_deltaTime,
					0.0F,
					kMaximumFrameTime);
				return;
			}
			if (jumpLandRequired) {
				const auto positionBeforeLanding = actor->GetPosition();
				bool graphJumpingBeforeLanding = false;
				const bool graphJumpAvailableBeforeLanding =
					actor->GetGraphVariableBool(
						inJumpStateVariable,
						graphJumpingBeforeLanding);
				// The grounded checkpoint and JumpLand describe one boundary. Clear
				// interpolation backlog before changing the persistent graph state,
				// otherwise the landing branch can begin while the puppet is aloft.
				actor->SetPosition(a_npc.targetPosition, true);
				if (animationState->rejectedEventAttempts == 0) {
					logs::info(
						"Animation sync aligned landing transform: actor={:08X} current=({:.3f},{:.3f},{:.3f}) target=({:.3f},{:.3f},{:.3f}) zError={:.3f} physicalJump={} physicalMidair={} graphJumpAvailable={} graphJump={}",
						actorHandle.native_handle(),
						positionBeforeLanding.x,
						positionBeforeLanding.y,
						positionBeforeLanding.z,
						a_npc.targetPosition.x,
						a_npc.targetPosition.y,
						a_npc.targetPosition.z,
						a_npc.targetPosition.z - positionBeforeLanding.z,
						actor->IsInJumpState(),
						actor->IsInMidair(),
						graphJumpAvailableBeforeLanding,
						graphJumpingBeforeLanding);
				}
			}

			bool speedAccepted = false;
			bool sampledSpeedAccepted = false;
			bool dampedSpeedAccepted = false;
			bool directionAccepted = false;
			bool turnAccepted = false;
			bool sprintVariableAccepted = false;
			bool jumpVariableAccepted = false;
			if (strings) {
				speedAccepted = actor->SetGraphVariableFloat(
					strings->speed,
					graphSpeed);
				sampledSpeedAccepted = actor->SetGraphVariableFloat(
					strings->speedSampled,
					graphSpeed);
				dampedSpeedAccepted = actor->SetGraphVariableFloat(
					speedDampedVariable,
					graphSpeed);
				directionAccepted = actor->SetGraphVariableFloat(
					strings->direction,
					isMoving ? sampleDirection : 0.0F);
				turnAccepted = actor->SetGraphVariableFloat(
					strings->turnDelta,
					0.0F);
				sprintVariableAccepted = actor->SetGraphVariableBool(
					strings->isSprinting,
					isSprinting);
				jumpVariableAccepted = actor->SetGraphVariableBool(
					inJumpStateVariable,
					state.jumping);
			}
			const bool variablesAccepted = stringsAvailable && speedAccepted &&
				sampledSpeedAccepted && dampedSpeedAccepted && directionAccepted &&
				turnAccepted && sprintVariableAccepted && jumpVariableAccepted;
			if (animationState->hasDispatched &&
				animationState->hasVariableUpdateResult &&
				animationState->lastVariableUpdateAccepted != variablesAccepted) {
				logs::info(
					"Animation sync graph variable refresh changed: actor={:08X} revision={} state={} result={} strings={} speed={} sampledSpeed={} dampedSpeed={} direction={} turn={} sprint={} jump={}",
					actorHandle.native_handle(),
					static_cast<unsigned long long>(desiredRevision),
					Multiplayer::GetMovementStateName(state.movementState),
					variablesAccepted ? "accepted" : "rejected",
					stringsAvailable,
					speedAccepted,
					sampledSpeedAccepted,
					dampedSpeedAccepted,
					directionAccepted,
					turnAccepted,
					sprintVariableAccepted,
					jumpVariableAccepted);
			}
			animationState->lastVariableUpdateAccepted = variablesAccepted;
			animationState->hasVariableUpdateResult = true;
			if (!eventDispatchRequired && variablesAccepted) {
				animationState->processedRevision.store(
					desiredRevision,
					std::memory_order_release);
			}
			if (eventDispatchRequired) {
				// Takeoff, landing, and locomotion start edges are evaluated against
				// the new persistent graph variables.
				if (moveRequired && isMoving && variablesAccepted) {
					moveAccepted = SendNetworkAnimationEvent(a_npc, moveStartEvent);
				}
				if (sneakRequired && isSneaking && variablesAccepted) {
					sneakAccepted = SendNetworkAnimationEvent(a_npc, sneakStartEvent);
				}
				if (sprintRequired && isSprinting && variablesAccepted) {
					sprintAccepted = SendNetworkAnimationEvent(
						a_npc,
						sprintStartEvent);
				}
				if (jumpRequired && variablesAccepted) {
					if (jumpLandRequired) {
						ArmJumpLandEndObservation(*actor);
					}
					jumpAccepted = SendNetworkAnimationEvent(
						a_npc,
						state.jumping ?
							(isMoving ?
								jumpDirectionalStartEvent :
								jumpStandingStartEvent) :
							jumpLandEvent);
				}
				const bool eventsAccepted =
					(!moveRequired || moveAccepted) &&
					(!sneakRequired || sneakAccepted) &&
					(!sprintRequired || sprintAccepted) &&
					(!jumpRequired || jumpAccepted);
				const auto variableAttempt =
					animationState->failedDispatchAttempts + 1;
				const auto eventAttempt =
					animationState->rejectedEventAttempts + 1;
				const bool eventAttemptsExhausted = !eventsAccepted &&
					eventAttempt >= kMaximumEventDispatchAttempts &&
					animationState->rejectedEventElapsed >= kMinimumEventRetrySeconds;
				const bool stateProcessed = variablesAccepted &&
					(eventsAccepted || eventAttemptsExhausted);
				if (variablesAccepted || variableAttempt == 1 ||
					variableAttempt % 60 == 0) {
					logs::info(
						"Animation sync graph dispatch: actor={:08X} revision={} previous={}/{} desired={}/{} result={} eventsAccepted={} strings={} speedVariable={} sampledSpeedVariable={} dampedSpeedVariable={} directionVariable={} turnVariable={} sprintVariable={} jumpVariable={} moveEvent={}/{} sneakEvent={}/{} sprintEvent={}/{} jumpEvent={}/{} graphSpeed={:.1f} variableAttempt={} eventAttempt={}/{}",
						actorHandle.native_handle(),
						static_cast<unsigned long long>(desiredRevision),
						hadPreviousState ?
							Multiplayer::GetMovementStateName(
								previousState.movementState) :
							"uninitialized",
						hadPreviousState && previousState.jumping ?
							"jumping" : "grounded",
						Multiplayer::GetMovementStateName(state.movementState),
						state.jumping ? "jumping" : "grounded",
						!variablesAccepted ? "variables-rejected" :
							eventsAccepted ? "processed" :
							eventAttemptsExhausted ? "processed-event-timeout" :
							"retrying-events",
						eventsAccepted,
						stringsAvailable,
						speedAccepted,
						sampledSpeedAccepted,
						dampedSpeedAccepted,
						directionAccepted,
						turnAccepted,
						sprintVariableAccepted,
						jumpVariableAccepted,
						moveAccepted,
						moveRequired,
						sneakAccepted,
						sneakRequired,
						sprintAccepted,
						sprintRequired,
						jumpAccepted,
						jumpRequired,
						graphSpeed,
						variableAttempt,
						eventAttempt,
						kMaximumEventDispatchAttempts);
				}

				if (stateProcessed) {
					animationState->lastDispatched = state;
					animationState->hasDispatched = true;
					if (reassertLocomotionAfterWeaponTransition) {
						if (weaponReplayNeedsFollowup) {
							animationState->weaponReplayBaseDispatched = true;
							logs::info(
								"Animation sync staged weapon locomotion replay: actor={:08X} step={}/{} target={}/{} weaponDrawn={} eventTimedOut={}",
								actorHandle.native_handle(),
								Multiplayer::GetMovementStateName(state.movementState),
								state.jumping ? "jumping" : "grounded",
								Multiplayer::GetMovementStateName(
									authoritativeState.movementState),
								authoritativeState.jumping ? "jumping" : "grounded",
								authoritativeState.weaponDrawn,
								eventAttemptsExhausted);
						} else {
							const bool weaponCompletionWasObserved =
								animationState->weaponTransitionCompletionObserved;
							const auto weaponTransitionWaitElapsed =
								animationState->weaponTransitionWaitElapsed;
							animationState->weaponLocomotionReassertPending = false;
							animationState->weaponTransitionCompletionObserved = false;
							animationState->weaponTransitionWaitElapsed = 0.0F;
							animationState->weaponTransitionAnimationObserved = false;
							animationState->weaponTransitionExitForced = false;
							animationState->weaponDefaultStateDispatched = false;
							animationState->weaponReplayBaseDispatched = false;
							animationState->weaponTransitionMoveStartDispatched = false;
							animationState->weaponReplayDiagnosticFramesRemaining = 30;
							animationState->weaponRestoredWithoutCompletion =
								!weaponCompletionWasObserved;
							animationState->weaponReplayDiagnosticWeaponDrawn =
								authoritativeState.weaponDrawn;
							logs::info(
								"Animation sync restored state after weapon transition: actor={:08X} state={} jumping={} weaponDrawn={} completionMarkerObserved={} weaponState={} transitionWaitSeconds={:.3f} eventTimedOut={}",
								actorHandle.native_handle(),
								Multiplayer::GetMovementStateName(state.movementState),
								state.jumping,
								state.weaponDrawn,
								weaponCompletionWasObserved,
								static_cast<std::underlying_type_t<RE::WEAPON_STATE>>(
									weaponState),
								weaponTransitionWaitElapsed,
								eventAttemptsExhausted);
						}
					}
					if (reassertLocomotionAfterSwimStop) {
						animationState->swimLocomotionReassertPending = false;
						logs::info(
							"Animation sync reasserted locomotion after swim stop: actor={:08X} state={}",
							actorHandle.native_handle(),
							Multiplayer::GetMovementStateName(state.movementState));
					}
					if (reassertLocomotionAfterAttack) {
						animationState->attackLocomotionReassertPending = false;
						logs::info(
							"Animation sync reasserted locomotion after normal attack: actor={:08X} state={}",
							actorHandle.native_handle(),
							Multiplayer::GetMovementStateName(state.movementState));
					}
					animationState->failedDispatchAttempts = 0;
					animationState->rejectedEventAttempts = 0;
					animationState->rejectedEventElapsed = 0.0F;
					if (jumpLandRequired) {
						animationState->landingCompletionPending = true;
						animationState->landingCompletionWaitFrames = 0;
						animationState->landingCompletionWaitElapsed = 0.0F;
						logs::info(
							"Animation sync waiting for landing completion marker: actor={:08X} revision={}",
							actorHandle.native_handle(),
							static_cast<unsigned long long>(desiredRevision));
					}
					animationState->processedRevision.store(
						desiredRevision,
						std::memory_order_release);
				} else if (!variablesAccepted) {
					++animationState->failedDispatchAttempts;
				} else {
					++animationState->rejectedEventAttempts;
					animationState->rejectedEventElapsed += std::clamp(
						a_deltaTime,
						0.0F,
						kMaximumFrameTime);
				}
			}

			animationState->directionCrossBlendElapsed += std::clamp(
				a_deltaTime,
				0.0F,
				kMaximumFrameTime);
			if (animationState->directionCrossBlendPending) {
				// One graph update after CyclicFreeze, matching Skyrim's own spacing.
				animationState->directionCrossBlendPending = false;
				(void) SendNetworkAnimationEvent(a_npc, cyclicCrossBlendEvent);
			} else if (!isMoving || state.jumping || !variablesAccepted) {
				animationState->hasCrossBlendedDirection = false;
			} else {
				const float directionChange = std::abs(
					sampleDirection - animationState->crossBlendedDirection);
				const float cyclicDirectionChange =
					(std::min)(directionChange, 1.0F - directionChange);
				if (!animationState->hasCrossBlendedDirection ||
					(cyclicDirectionChange >= kDirectionCrossBlendThreshold &&
						animationState->directionCrossBlendElapsed >=
							kDirectionCrossBlendIntervalSeconds)) {
					(void) SendNetworkAnimationEvent(a_npc, cyclicFreezeEvent);
					animationState->crossBlendedDirection = sampleDirection;
					animationState->hasCrossBlendedDirection = true;
					animationState->directionCrossBlendPending = true;
					animationState->directionCrossBlendElapsed = 0.0F;
				}
			}

			// Let the native transition start only after this checkpoint's persistent
			// speed and locomotion events have settled. Once active, moveStart selects
			// the equip graph's built-in locomotion blend without overriding its sync
			// state variable.
			if (IsAnimationStateSettled(a_npc)) {
				ApplyAuthoritativeWeaponPresentation(
					*actor,
					a_npc,
					isMoving,
					moveRequired && isMoving && moveAccepted);
			}
		}

		RE::SIT_SLEEP_STATE GetActorSitSleepState(const RE::Actor& a_actor)
		{
			// Actor's ActorState base moved on AE. Calling ActorState members directly
			// on RE::Actor uses the SE base offset; AsActorState relocates it.
			const auto* actorState = a_actor.AsActorState();
			return actorState ? actorState->GetSitSleepState() :
				RE::SIT_SLEEP_STATE::kNormal;
		}

		bool SetActorSitSleepState(
			RE::Actor& a_actor,
			RE::SIT_SLEEP_STATE a_state)
		{
			auto* actorState = a_actor.AsActorState();
			return actorState && actorState->DoSetSitSleepState(a_state);
		}

		constexpr bool IsFurnitureEntryComplete(RE::SIT_SLEEP_STATE a_state)
		{
			return a_state == RE::SIT_SLEEP_STATE::kIsSitting ||
				a_state == RE::SIT_SLEEP_STATE::kWantToStand ||
				a_state == RE::SIT_SLEEP_STATE::kIsSleeping ||
				a_state == RE::SIT_SLEEP_STATE::kWantToWake;
		}

		RE::FormID GetOccupiedFurnitureRefID(RE::Actor& a_actor)
		{
			auto furniture = a_actor.GetOccupiedFurniture().get();
			return furniture ? furniture->GetFormID() : 0;
		}

		bool IsSameFurnitureCoordinateSpace(
			const RE::TESObjectREFR& a_actor,
			const RE::TESObjectREFR& a_furniture)
		{
			const auto* actorWorldspace = a_actor.GetWorldspace();
			const auto* furnitureWorldspace = a_furniture.GetWorldspace();
			if (actorWorldspace || furnitureWorldspace) {
				return actorWorldspace && actorWorldspace == furnitureWorldspace;
			}
			return a_actor.GetParentCell() &&
				a_actor.GetParentCell() == a_furniture.GetParentCell();
		}

		// The furniture engine calls below are Address Library IDs verified only
		// against 1.7.104. Other runtimes never find a marker and never start entry.

		std::int32_t FindAvailableFurnitureMarker(
			RE::Actor& a_actor,
			RE::TESObjectREFR& a_furniture)
		{
			if (!REL::Module::IsAE()) {
				return -1;
			}
			using func_t = std::int32_t(RE::TESObjectREFR*, RE::Actor*, bool);
			static REL::Relocation<func_t> func{ REL::ID(20162) };
			return func(std::addressof(a_furniture), std::addressof(a_actor), true);
		}

		std::optional<RE::NiPoint3> GetFurnitureMarkerPosition(
			const RE::TESObjectREFR& a_furniture,
			std::int32_t a_marker)
		{
			auto* root = a_furniture.Get3D2();
			const auto* markerNode = root ?
				RE::BSFurnitureMarkerNode::FindBSFurnitureMarkerNode(root) :
				nullptr;
			if (!markerNode || a_marker < 0 ||
				static_cast<std::uint32_t>(a_marker) >= markerNode->markers.size()) {
				return std::nullopt;
			}
			return root->world * markerNode->markers[a_marker].offset;
		}

		bool StartNativeFurnitureEntry(
			ControlledNPC& a_npc,
			RE::Actor& a_actor,
			RE::TESObjectREFR& a_furniture,
			float a_markerDistance)
		{
			auto* process = a_actor.GetActorRuntimeData().currentProcess;
			if (!REL::Module::IsAE() || !process || !process->middleHigh) {
				return false;
			}

			// TESFurniture::Activate rejects every activator except the player, so use
			// the entry NPC Sit procedures use (ID 37330). It records the occupied
			// reference, lets ID 39923 choose an entry point (filling furnitureIdle,
			// furniturePathPoint and the entry angle) and reserves that marker.
			// UpdateNativeSitSleepState then plays the chosen entry idle. The quick
			// entry (ID 37324) is not used: it selects the Idle*EnterInstant records
			// and skips the transition.
			using UseFurniture = bool(RE::Actor*, RE::TESObjectREFR*);
			static REL::Relocation<UseFurniture> useFurniture{ REL::ID(37330) };

			(void) SetActorSitSleepState(a_actor, RE::SIT_SLEEP_STATE::kNormal);
			const bool started =
				useFurniture(std::addressof(a_actor), std::addressof(a_furniture));
			const auto* entryIdle = process->middleHigh->furnitureIdle;
			if (!started) {
				// Occupancy is recorded before the entry point is chosen and is left
				// behind when none is available.
				process->ClearFurniture();
			}
			logs::info(
				"Animation sync started furniture entry: actor={:08X} furniture={:08X} started={} marker={} markerDistance={:.2f} entryIdle={:08X}",
				a_npc.handle.native_handle(),
				a_furniture.GetFormID(),
				started,
				process->middleHigh->currentFurnitureMarkerID,
				a_markerDistance,
				started && entryIdle ? entryIdle->GetFormID() : 0);
			return started;
		}

		void UpdateNativeSitSleepState(RE::Actor& a_actor)
		{
			// Actor::UpdateSitSleepState (ID 37332 -> AIProcess ID 39861) advances
			// Normal -> WantToSit -> WaitingForSitAnim and WantToStand -> Normal.
			// Skyrim calls it from package procedure branches that the puppet's
			// do-nothing package never reaches.
			if (!REL::Module::IsAE()) {
				return;
			}
			using func_t = void(RE::Actor*);
			static REL::Relocation<func_t> func{ REL::ID(37332) };
			func(std::addressof(a_actor));
		}

		void ResetFurnitureRequest(ControlledNPC& a_npc)
		{
			a_npc.requestedFurnitureRefID = 0;
			a_npc.furnitureRetryRemaining = 0.0F;
			a_npc.furnitureTransitionElapsed = 0.0F;
		}

		bool AdvanceFurnitureInteraction(
			RE::Actor& a_actor,
			ControlledNPC& a_npc,
			float a_deltaTime)
		{
			const auto targetFurnitureRefID = a_npc.targetFurnitureRefID;
			const auto occupiedFurnitureRefID =
				GetOccupiedFurnitureRefID(a_actor);
			const float deltaTime = std::clamp(
				a_deltaTime,
				0.0F,
				kMaximumFrameTime);
			a_npc.furnitureRetryRemaining -= deltaTime;
			const auto sitSleepState = GetActorSitSleepState(a_actor);
			if (sitSleepState != a_npc.loggedSitSleepState) {
				logs::info(
					"Animation sync puppet sit/sleep state changed: actor={:08X} previous={} current={} occupied={:08X} target={:08X}",
					a_npc.handle.native_handle(),
					static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(
						a_npc.loggedSitSleepState),
					static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(
						sitSleepState),
					occupiedFurnitureRefID,
					targetFurnitureRefID);
				a_npc.loggedSitSleepState = sitSleepState;
			}
			if (occupiedFurnitureRefID != 0) {
				UpdateNativeSitSleepState(a_actor);
			}

			// Entry, use, and the furniture graph are driven natively once the target
			// is occupied.
			if (targetFurnitureRefID != 0 &&
				occupiedFurnitureRefID == targetFurnitureRefID) {
				SetFurnitureAnimationOwnership(a_npc, true);
				a_npc.requestedFurnitureRefID = targetFurnitureRefID;
				a_npc.furnitureRetryRemaining = 0.0F;
				a_npc.furnitureTransitionElapsed = 0.0F;
				return true;
			}

			if (targetFurnitureRefID != 0 &&
				a_npc.requestedFurnitureRefID == targetFurnitureRefID &&
				occupiedFurnitureRefID == 0) {
				// Skyrim dropped a started entry. Hold ownership for a bounded time
				// before releasing the request for a fresh attempt.
				SetFurnitureAnimationOwnership(a_npc, true);
				a_npc.furnitureTransitionElapsed += deltaTime;
				if (a_npc.furnitureTransitionElapsed <
					kFurnitureEntryFallbackSeconds) {
					return true;
				}

				logs::warn(
					"Animation sync abandoned stuck furniture entry for retry: actor={:08X} furniture={:08X} sitSleepState={} elapsed={:.3f}",
					a_npc.handle.native_handle(),
					targetFurnitureRefID,
					static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(
						sitSleepState),
					a_npc.furnitureTransitionElapsed);
				a_actor.InitiateDoNothingPackage();
				ResetFurnitureRequest(a_npc);
				SetFurnitureAnimationOwnership(a_npc, false);
				return false;
			}

			const bool mustExitCurrentFurniture =
				occupiedFurnitureRefID != 0 ||
				a_npc.requestedFurnitureRefID != 0;
			const bool furnitureAnimationOwned = a_npc.animationState &&
				a_npc.animationState->furnitureAnimationOwned;
			if (mustExitCurrentFurniture ||
				(targetFurnitureRefID == 0 && furnitureAnimationOwned)) {
				SetFurnitureAnimationOwnership(a_npc, true);
				if (occupiedFurnitureRefID == 0 &&
					GetActorSitSleepState(a_actor) == RE::SIT_SLEEP_STATE::kNormal) {
					const auto exitedFurnitureRefID =
						a_npc.requestedFurnitureRefID;
					a_actor.InitiateDoNothingPackage();
					ResetFurnitureRequest(a_npc);
					SetFurnitureAnimationOwnership(a_npc, false);
					if (a_npc.animationState) {
						// Furniture graphs do not consistently return to the locomotion
						// variables which preceded entry. Reconstruct the current checkpoint.
						InvalidateAnimationDispatch(*a_npc.animationState);
					}
					const auto* actorState = a_actor.AsActorState();
					if (a_npc.targetAnimationState.weaponDrawn && actorState &&
						actorState->GetWeaponState() == RE::WEAPON_STATE::kSheathed) {
						// Sitting sheathes the weapon, but checkpoints are not staged while
						// furniture owns the graph, so the request still holds the pre-entry
						// drawn state. Dispatching it now would draw on the reconstruction
						// update and then sheathe once the fresh checkpoint is staged.
						auto sheathedState = a_npc.targetAnimationState;
						sheathedState.weaponDrawn = false;
						SetAnimationState(a_npc, sheathedState);
					}
					logs::info(
						"Animation sync completed furniture exit: actor={:08X} furniture={:08X} nextFurniture={:08X}",
						a_npc.handle.native_handle(),
						exitedFurnitureRefID,
						targetFurnitureRefID);
					return false;
				}

				if (occupiedFurnitureRefID != 0 &&
					!IsFurnitureEntryComplete(sitSleepState)) {
					// The entry never reached a seated state, so there is no native exit
					// to request. Release the reservation; the next update completes exit.
					if (auto* process =
							a_actor.GetActorRuntimeData().currentProcess) {
						process->ClearFurniture();
					}
					(void) SetActorSitSleepState(
						a_actor,
						RE::SIT_SLEEP_STATE::kNormal);
					logs::info(
						"Animation sync cancelled incomplete furniture entry: actor={:08X} occupied={:08X} target={:08X} sitSleepState={}",
						a_npc.handle.native_handle(),
						occupiedFurnitureRefID,
						targetFurnitureRefID,
						static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(
							sitSleepState));
					return true;
				}

				a_npc.furnitureTransitionElapsed += deltaTime;
				if (a_npc.furnitureRetryRemaining <= 0.0F) {
					const bool sleeping =
						sitSleepState == RE::SIT_SLEEP_STATE::kIsSleeping ||
						sitSleepState == RE::SIT_SLEEP_STATE::kWantToWake;
					a_actor.InitiateGetUpPackage();
					const bool stateAccepted = SetActorSitSleepState(
						a_actor,
						sleeping ? RE::SIT_SLEEP_STATE::kWantToWake :
							RE::SIT_SLEEP_STATE::kWantToStand);
					static const RE::BSFixedString furnitureExitEvent{
						"IdleFurnitureExit"
					};
					const bool eventAccepted = NotifyNetworkAnimationGraph(
						a_actor,
						furnitureExitEvent);
					a_npc.furnitureRetryRemaining = kFurnitureRetryInterval;
					logs::info(
						"Animation sync requested furniture exit: actor={:08X} occupied={:08X} target={:08X} sitSleepState={} stateAccepted={} eventAccepted={}",
						a_npc.handle.native_handle(),
						occupiedFurnitureRefID,
						targetFurnitureRefID,
						static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(
							sitSleepState),
						stateAccepted,
						eventAccepted);
				}

				if (a_npc.furnitureTransitionElapsed >=
					kFurnitureExitFallbackSeconds) {
					if (auto* process =
							a_actor.GetActorRuntimeData().currentProcess) {
						process->ClearFurniture();
					}
					(void) SetActorSitSleepState(
						a_actor,
						RE::SIT_SLEEP_STATE::kNormal);
					static const RE::BSFixedString defaultStateEvent{
						"IdleForceDefaultState"
					};
					(void) NotifyNetworkAnimationGraph(
						a_actor,
						defaultStateEvent);
					a_actor.InitiateDoNothingPackage();
					a_npc.furnitureTransitionElapsed = 0.0F;
					logs::warn(
						"Animation sync forced stuck furniture exit: actor={:08X} occupied={:08X} target={:08X}",
						a_npc.handle.native_handle(),
						occupiedFurnitureRefID,
						targetFurnitureRefID);
				}
				return true;
			}
			if (targetFurnitureRefID == 0) {
				return false;
			}

			// The target is a FormID resolved locally; Skyrim chooses the entry point,
			// idle, and marker for the puppet.
			auto* furniture = RE::TESForm::LookupByID<RE::TESObjectREFR>(
				targetFurnitureRefID);
			const auto* furnitureBase = furniture ? furniture->GetBaseObject() : nullptr;
			const bool isFurniture = furnitureBase &&
				furnitureBase->As<RE::TESFurniture>();
			const bool loaded3D = furniture && furniture->Is3DLoaded();
			const bool sameCoordinateSpace = furniture &&
				IsSameFurnitureCoordinateSpace(a_actor, *furniture);
			if (!furniture || !isFurniture || !loaded3D ||
				!sameCoordinateSpace) {
				if (a_npc.furnitureRetryRemaining <= 0.0F) {
					a_npc.furnitureRetryRemaining = kFurnitureRetryInterval;
					logs::warn(
						"Animation sync waiting for furniture reference: actor={:08X} furniture={:08X} resolved={} furnitureBase={} loaded3D={} sameSpace={}",
						a_npc.handle.native_handle(),
						targetFurnitureRefID,
						furniture != nullptr,
						isFurniture,
						loaded3D,
						sameCoordinateSpace);
				}
				// The latest transform remains authoritative while the reference is
				// unavailable. Do not strand the puppet at its pre-interaction position.
				return false;
			}
			if (!a_npc.ready ||
				!IsAnimationStateSettled(a_npc) ||
				(a_npc.animationState &&
					(a_npc.animationState->attackPlaybackPending ||
						a_npc.animationState->weaponLocomotionReassertPending ||
						a_npc.animationState->graphRecoveryInProgress)) ||
				a_npc.furnitureRetryRemaining > 0.0F) {
				return false;
			}

			// The source reports occupancy when it activates, then travels to the entry
			// point before its entry idle moves it onto the marker. Waiting for the
			// source to settle on the marker delays the puppet by the whole entry idle.
			// Start once the puppet follows the source close to the furniture; the
			// native entry aligns it to the chosen entry point.
			const auto actorPosition = a_actor.GetPosition();
			if ((a_npc.targetPosition - actorPosition).Length() >
				kFurnitureEntryAlignmentDistance) {
				return false;
			}
			const auto marker = FindAvailableFurnitureMarker(a_actor, *furniture);
			if (marker < 0) {
				a_npc.furnitureRetryRemaining = kFurnitureRetryInterval;
				logs::warn(
					"Animation sync found no available furniture marker: actor={:08X} furniture={:08X}",
					a_npc.handle.native_handle(),
					targetFurnitureRefID);
				return false;
			}
			const auto markerPosition =
				GetFurnitureMarkerPosition(*furniture, marker).value_or(
					furniture->GetPosition());
			const float markerDistance = std::hypot(
				markerPosition.x - actorPosition.x,
				markerPosition.y - actorPosition.y);
			if (markerDistance > kFurnitureEntryMarkerDistance) {
				return false;
			}

			// Remove residual interpolation error before Skyrim aligns the actor.
			a_actor.SetPosition(a_npc.targetPosition, true);
			if (std::isfinite(a_npc.targetHeading)) {
				a_actor.SetHeading(a_npc.targetHeading);
			}
			SetFurnitureAnimationOwnership(a_npc, true);
			a_npc.furnitureRetryRemaining = kFurnitureRetryInterval;
			if (!StartNativeFurnitureEntry(
					a_npc,
					a_actor,
					*furniture,
					markerDistance)) {
				// Do not freeze translation or leave the graph open to local events.
				SetFurnitureAnimationOwnership(a_npc, false);
				return false;
			}
			a_npc.requestedFurnitureRefID = targetFurnitureRefID;
			a_npc.furnitureTransitionElapsed = 0.0F;
			return true;
		}
	}

	bool BindAuthoritativeGameThread()
	{
		const auto currentThread = REX::W32::GetCurrentThreadId();
		std::uint32_t expectedThread = 0;
		if (s_authoritativeGameThreadID.compare_exchange_strong(
				expectedThread,
				currentThread,
				std::memory_order_acq_rel,
				std::memory_order_acquire)) {
			logs::info(
				"Bound remote-puppet engine access to main-loop thread {}",
				currentThread);
			return true;
		}

		if (expectedThread == currentThread) {
			return true;
		}

		if (!s_gameThreadMigrationLogged.exchange(
				true,
				std::memory_order_relaxed)) {
			logs::critical(
				"Main-loop hook migrated threads; refusing remote-puppet update: currentThread={} authoritativeThread={}",
				currentThread,
				expectedThread);
		}
		return false;
	}

	void InstallRemoteReplicaHooks()
	{
		// NPC references are Character instances. Their first four vtables belong
		// to TESObjectREFR's four polymorphic bases. Index 3 is the
		// IAnimationGraphManagerHolder subobject, whose vfunc 1 is
		// NotifyAnimationGraph.
		REL::Relocation<std::uintptr_t> characterAnimationGraphVTable{
			RE::VTABLE_Character[3]
		};
		PuppetAnimationGraphHook::_original =
			characterAnimationGraphVTable.write_vfunc(
				0x1,
				PuppetAnimationGraphHook::Thunk);
		REL::Relocation<std::uintptr_t> directionChannelVTable{
			RE::VTABLE_BSTAnimationGraphDataChannel_Actor_float_ActorDirectionChannel_[0]
		};
		PuppetDirectionChannelHook::_original =
			directionChannelVTable.write_vfunc(
				0x1,
				PuppetDirectionChannelHook::Thunk);

		REL::Relocation<std::uintptr_t> characterVTable{ RE::VTABLE_Character[0] };
		const auto actorUpdateIndex = REL::Relocate(0xAD, 0xAD, 0xAF);
		RemoteActorUpdateHook::_original = characterVTable.write_vfunc(
			actorUpdateIndex,
			RemoteActorUpdateHook::Thunk);

		// TESObjectREFR contributes vtables 0-3. Character vtable 4 is the
		// MagicTarget subobject, whose vfunc 1 attaches a new ActiveEffect.
		REL::Relocation<std::uintptr_t> characterMagicTargetVTable{
			RE::VTABLE_Character[4]
		};
		PuppetMagicTargetHook::_original =
			characterMagicTargetVTable.write_vfunc(
				0x1,
				PuppetMagicTargetHook::Thunk);

		const auto handleHealthDamageIndex = REL::Relocate(0x104, 0x104, 0x106);
		PuppetHealthDamageHook::_original = characterVTable.write_vfunc(
			handleHealthDamageIndex,
			PuppetHealthDamageHook::Thunk);

		const auto killDyingIndex = REL::Relocate(0xAA, 0xAA, 0xAC);
		PuppetKillDyingHook::_original = characterVTable.write_vfunc(
			killDyingIndex,
			PuppetKillDyingHook::Thunk);

		const auto killIndex = REL::Relocate(0x10E, 0x10E, 0x110);
		PuppetKillHook::_original = characterVTable.write_vfunc(
			killIndex,
			PuppetKillHook::Thunk);

		// Character vtable 5 is its ActorValueOwner subobject. This catches health
		// damage applied directly by traps, scripts, hazards, or engine paths that do
		// not pass through Actor::HandleHealthDamage.
		REL::Relocation<std::uintptr_t> characterActorValueOwnerVTable{
			RE::VTABLE_Character[5]
		};
		PuppetActorValueHook::_original =
			characterActorValueOwnerVTable.write_vfunc(
				0x6,
				PuppetActorValueHook::Thunk);

		logs::info(
			"Installed remote-replica actor policy, damage/kill firewall, magic firewall, and animation firewall");
	}

	void RegisterRemoteReplicaEventSinks()
	{
		static bool registered = false;
		if (registered) {
			return;
		}

		auto* eventSources = RE::ScriptEventSourceHolder::GetSingleton();
		if (!eventSources) {
			logs::critical("Unable to register the remote-replica hit event sink");
			return;
		}

		eventSources->AddEventSink(PuppetHitEventSink::GetSingleton());
		eventSources->AddEventSink(PuppetMagicEffectEventSink::GetSingleton());
		registered = true;
		logs::info("Registered remote-replica hit and magic observation event sinks");
	}

	std::optional<ControlledNPC> SpawnControlledNPC(
		RE::TESNPC* a_npcBase,
		const RE::NiPoint3& a_targetPosition,
		float a_targetHeading,
		Multiplayer::AnimationState a_animationState,
		float a_speed,
		float a_stoppingDistance)
	{
		if (!RequireGameThread("SpawnControlledNPC") ||
			!a_npcBase ||
			!std::isfinite(a_targetPosition.x) ||
			!std::isfinite(a_targetPosition.y) ||
			!std::isfinite(a_targetPosition.z) ||
			!std::isfinite(a_targetHeading) ||
			!std::isfinite(a_speed) || a_speed <= 0.0F ||
			!std::isfinite(a_stoppingDistance) || a_stoppingDistance < 0.0F) {
			return std::nullopt;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* dataHandler = RE::TESDataHandler::GetSingleton();
		if (!player || !player->GetParentCell() || !dataHandler) {
			return std::nullopt;
		}

		auto* targetCell = player->GetParentCell();
		auto* worldspace = player->GetWorldspace();
		if (worldspace) {
			auto* tes = RE::TES::GetSingleton();
			targetCell = tes ? tes->GetCell(a_targetPosition) : nullptr;
			if (!targetCell) {
				// Wait until the exterior cell containing the first remote snapshot
				// is loaded instead of briefly creating the puppet beside the player.
				return std::nullopt;
			}
		}

		// Puppets must survive ordinary exterior cell unloads within one worldspace.
		// Interior/worldspace changes are handled as explicit respawn boundaries by
		// FrameUpdate. Create the reference at the first compatible snapshot so its
		// initial character-controller position is never the local player's position.
		auto* constructionBase = a_npcBase;
		if (a_npcBase->IsPlayer()) {
			constructionBase = RE::TESForm::LookupByID<RE::TESNPC>(
				kActorConstructionTemplateFormID);
			if (!constructionBase) {
				logs::critical(
					"Unable to find remote-puppet actor construction template {:08X}",
					kActorConstructionTemplateFormID);
				return std::nullopt;
			}
		}

		const RE::NiPoint3 targetRotation{ 0.0F, 0.0F, a_targetHeading };
		const auto spawnedHandle = dataHandler->CreateReferenceAtLocation(
			constructionBase,
			a_targetPosition,
			targetRotation,
			targetCell,
			worldspace,
			nullptr,
			nullptr,
			RE::ObjectRefHandle{},
			true,
			true);
		auto spawnedRef = spawnedHandle.get();
		if (!spawnedRef) {
			return std::nullopt;
		}

		auto* actor = spawnedRef->As<RE::Actor>();
		if (!actor) {
			return std::nullopt;
		}

		if (constructionBase != a_npcBase) {
			// CreateReferenceAtLocation does return a reference for PlayerBase, but it
			// never completes normal actor/3D initialization. Constructing from a
			// regular TESNPC first avoids that special-case while SetObjectReference
			// still makes the resulting actor use the player's race and appearance.
			const bool rebuildLoaded3D = actor->Is3DLoaded();
			actor->SetObjectReference(a_npcBase);
			if (actor->GetActorBase() != a_npcBase) {
				logs::critical(
					"Unable to assign player base to remote puppet: actor={:08X} requestedBase={:08X} actualBase={:08X}",
					actor->GetHandle().native_handle(),
					a_npcBase->GetFormID(),
					actor->GetActorBase() ? actor->GetActorBase()->GetFormID() : 0);
				actor->Disable();
				actor->SetDelete(true);
				return std::nullopt;
			}
			if (rebuildLoaded3D) {
				// Usually the initial 3D load is still queued. If it completed
				// synchronously, discard the construction template's model now.
				actor->DoReset3D(false);
			}
		}

		actor->SetActivationBlocked(true);
		// The animation graph and character controller only finish initializing
		// while the actor remains in Skyrim's normal processing pipeline.
		actor->EnableAI(true);

		auto animationState = std::make_shared<AnimationState>();
		RequestAnimationUpdate(animationState, a_animationState, a_speed, 0.0F);
		logs::info(
			"Created remote-puppet actor reference: actor={:08X} base={:08X} constructionBase={:08X} revision={} desired={}/{}",
			actor->GetHandle().native_handle(),
			actor->GetActorBase() ? actor->GetActorBase()->GetFormID() : 0,
			constructionBase->GetFormID(),
			static_cast<unsigned long long>(animationState->desiredRevision.load()),
			Multiplayer::GetMovementStateName(a_animationState.movementState),
			a_animationState.jumping ? "jumping" : "grounded");

		ControlledNPC controlledNPC{
			.handle = actor->GetHandle(),
			.targetPosition = a_targetPosition,
			.targetHeading = a_targetHeading,
			.targetAnimationState = a_animationState,
			.animationState = std::move(animationState),
			.translationSpeed = a_speed,
			.animationSpeed = a_speed,
			.stoppingDistance = a_stoppingDistance,
			.animationGraphHolder = nullptr,
			.initializationWaitFrames = 0,
			.ready = false
		};
		RegisterPuppetGraph(controlledNPC, *actor);
		ApplyRemoteReplicaPolicy(*actor);
		return controlledNPC;
	}

	void SetTargetTransform(
		ControlledNPC& a_npc,
		const RE::NiPoint3& a_targetPosition,
		float a_targetHeading,
		float a_translationSpeed,
		float a_direction)
	{
		a_npc.targetPosition = a_targetPosition;
		a_npc.targetHeading = a_targetHeading;
		if (std::isfinite(a_translationSpeed) && a_translationSpeed >= 0.0F) {
			a_npc.translationSpeed = a_translationSpeed;
			a_npc.animationSpeed = a_translationSpeed;
			a_npc.animationDirection =
				Multiplayer::NormalizeMovementDirection(a_direction);
			RequestAnimationUpdate(
				a_npc.animationState,
				a_npc.targetAnimationState,
				a_npc.animationSpeed,
				a_npc.animationDirection);
			SetAuthoritativeDirection(a_npc);
		}
	}

	void SetAnimationState(
		ControlledNPC& a_npc,
		Multiplayer::AnimationState a_animationState)
	{
		const auto previousAnimationState = a_npc.targetAnimationState;
		a_npc.targetAnimationState = a_animationState;
		SetAuthoritativeJumpState(a_npc, a_animationState.jumping);
		SetAuthoritativeDirection(a_npc);
		SetAuthoritativeWeaponDrawnState(a_npc, a_animationState.weaponDrawn);
		RequestAnimationUpdate(
			a_npc.animationState,
			a_animationState,
			a_npc.animationSpeed,
			a_npc.animationDirection);
		if (previousAnimationState != a_animationState && a_npc.animationState) {
			logs::info(
				"Animation sync updated actor request: actor={:08X} revision={} previous={}/{} desired={}/{}",
				a_npc.handle.native_handle(),
				static_cast<unsigned long long>(
					a_npc.animationState->desiredRevision.load(std::memory_order_acquire)),
				Multiplayer::GetMovementStateName(
					previousAnimationState.movementState),
				previousAnimationState.jumping ? "jumping" : "grounded",
				Multiplayer::GetMovementStateName(a_animationState.movementState),
				a_animationState.jumping ? "jumping" : "grounded");
		}
	}

	void SetFurnitureState(
		ControlledNPC& a_npc,
		RE::FormID a_furnitureRefID)
	{
		if (!RequireGameThread("SetFurnitureState") ||
			a_npc.targetFurnitureRefID == a_furnitureRefID) {
			return;
		}

		const auto previousFurnitureRefID = a_npc.targetFurnitureRefID;
		a_npc.targetFurnitureRefID = a_furnitureRefID;
		a_npc.furnitureRetryRemaining = 0.0F;
		if (a_npc.requestedFurnitureRefID == 0) {
			// An in-progress exit keeps its elapsed time so that rapid target changes
			// cannot postpone the forced-exit fallback indefinitely.
			a_npc.furnitureTransitionElapsed = 0.0F;
		}
		logs::info(
			"Animation sync updated furniture request: actor={:08X} previous={:08X} desired={:08X}",
			a_npc.handle.native_handle(),
			previousFurnitureRefID,
			a_furnitureRefID);
	}

	bool IsFurnitureInteractionPlaying(const ControlledNPC& a_npc)
	{
		return a_npc.requestedFurnitureRefID != 0 ||
			(a_npc.animationState &&
				a_npc.animationState->furnitureAnimationOwned);
	}

	bool SendNetworkAnimationEvent(
		ControlledNPC& a_npc,
		const RE::BSFixedString& a_eventName)
	{
		if (!RequireGameThread("SendNetworkAnimationEvent")) {
			return false;
		}

		auto actorPtr = a_npc.handle.get();
		if (!actorPtr || !a_npc.animationGraphHolder) {
			return false;
		}

		auto* actor = actorPtr.get();
		auto* holder = static_cast<RE::IAnimationGraphManagerHolder*>(actor);
		if (holder != a_npc.animationGraphHolder) {
			return false;
		}

		return NotifyNetworkAnimationGraph(*actor, a_eventName);
	}

	bool IsAnimationStateSettled(const ControlledNPC& a_npc)
	{
		if (!a_npc.animationState) {
			return true;
		}

		return !a_npc.animationState->landingCompletionPending &&
			a_npc.animationState->processedRevision.load(
				std::memory_order_acquire) ==
			a_npc.animationState->desiredRevision.load(std::memory_order_acquire);
	}

	bool CanDispatchNormalAttack(const ControlledNPC& a_npc)
	{
		if (!RequireGameThread("CanDispatchNormalAttack") || !a_npc.ready ||
			!a_npc.animationState || !a_npc.animationGraphHolder ||
			!IsAnimationStateSettled(a_npc) ||
			a_npc.animationState->attackPlaybackPending ||
			a_npc.animationState->attackLocomotionReassertPending ||
			a_npc.animationState->weaponLocomotionReassertPending ||
			a_npc.animationState->graphRecoveryInProgress ||
			a_npc.targetAnimationState.jumping ||
			!a_npc.targetAnimationState.weaponDrawn) {
			return false;
		}

		auto actorPtr = a_npc.handle.get();
		if (!actorPtr) {
			return false;
		}
		auto* actor = actorPtr.get();
		auto* holder = static_cast<RE::IAnimationGraphManagerHolder*>(actor);
		const auto* actorState = actor->AsActorState();
		if (holder != a_npc.animationGraphHolder || !actorState ||
			actorState->GetWeaponState() != RE::WEAPON_STATE::kDrawn ||
			actorState->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone) {
			return false;
		}

		RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
		return actor->Is3DLoaded() &&
			actor->GetAnimationGraphManager(graphManager) && graphManager &&
			a_npc.animationState->graphManager == graphManager.get();
	}

	bool DispatchNormalAttack(ControlledNPC& a_npc)
	{
		if (!CanDispatchNormalAttack(a_npc)) {
			return false;
		}

		static const RE::BSFixedString attackStartEvent{ "attackStart" };
		auto actorPtr = a_npc.handle.get();
		if (!actorPtr) {
			return false;
		}
		auto& actorFlags = actorPtr->GetActorRuntimeData().boolFlags;
		// The persistent flag prevents autonomous combat decisions, but it can also
		// make the behavior graph reject a directly replicated attackStart. Open a
		// game-thread-only dispatch window and restore the policy before returning;
		// no AI update can run inside this synchronous call.
		actorFlags.reset(RE::Actor::BOOL_FLAGS::kAttackingDisabled);
		const bool accepted = SendNetworkAnimationEvent(a_npc, attackStartEvent);
		actorFlags.set(RE::Actor::BOOL_FLAGS::kAttackingDisabled);
		if (!accepted) {
			return false;
		}

		a_npc.animationState->attackPlaybackPending = true;
		a_npc.animationState->attackPlaybackObserved = false;
		a_npc.animationState->attackPlaybackElapsed = 0.0F;
		a_npc.animationState->attackLocomotionReassertPending = false;
		const auto* actorState = actorPtr->AsActorState();
		a_npc.animationState->attackPlaybackObserved = actorState &&
			actorState->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone;
		return true;
	}

	bool IsAnimationActionPlaying(const ControlledNPC& a_npc)
	{
		return a_npc.animationState &&
			a_npc.animationState->attackPlaybackPending;
	}

	void RemoveControlledNPC(ControlledNPC& a_npc)
	{
		if (!RequireGameThread("RemoveControlledNPC")) {
			return;
		}

		UnregisterPuppetGraph(a_npc);
		auto actorPtr = a_npc.handle.get();
		if (!actorPtr) {
			return;
		}

		auto* actor = actorPtr.get();
		actor->Disable();
		actor->SetDelete(true);
	}

	bool UpdateControlledNPC(
		ControlledNPC& a_npc,
		float a_deltaTime,
		bool a_allowTranslation)
	{
		if (!RequireGameThread("UpdateControlledNPC")) {
			return true;
		}

		auto actorPtr = a_npc.handle.get();
		if (!actorPtr) {
			UnregisterPuppetGraph(a_npc);
			return false;
		}

		auto* actor = actorPtr.get();
		if (a_npc.animationState) {
			AdvanceAttackPlayback(*actor, *a_npc.animationState, a_deltaTime);
		}
		ApplyRemoteReplicaPolicy(*actor);
		// Combat packages can be selected before the suppression flags are
		// reasserted, especially during town-wide alarm/flee evaluation. Mutate
		// package and weapon state only at this established main-loop boundary,
		// never from inside Skyrim's per-actor update hook.
		SanitizeRemoteCombatState(*actor);
		if (RecoverAuthoritativeAnimation(*actor, a_npc, a_deltaTime)) {
			// Keep transform and graph writes paused until the reverted graph and its
			// controller have remained available across consecutive main-loop passes.
			return true;
		}
		auto* controller = actor->GetCharController();

		// Reference creation may return before the actor has a loaded 3D and
		// character controller. Disabling AI at spawn time clears kProcessMe,
		// preventing that initialization and leaving the actor in a T-pose.
		if (!a_npc.ready) {
			RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
			const bool has3D = actor->Is3DLoaded();
			const bool hasController = controller != nullptr;
			const bool hasGraph = has3D && hasController &&
				actor->GetAnimationGraphManager(graphManager) && graphManager;
			if (!has3D || !hasController || !hasGraph) {
				++a_npc.initializationWaitFrames;
				if (a_npc.initializationWaitFrames == 1 ||
					a_npc.initializationWaitFrames %
						kActorInitializationLogInterval == 0) {
					logs::info(
						"Waiting for remote-puppet actor initialization: actor={:08X} base={:08X} loaded3D={} controller={} graph={} waitedFrames={}",
						a_npc.handle.native_handle(),
						actor->GetActorBase() ? actor->GetActorBase()->GetFormID() : 0,
						has3D,
						hasController,
						hasGraph,
						a_npc.initializationWaitFrames);
				}
				return true;
			}

			// Keep the actor in the normal update pipeline, but give it no autonomous
			// travel package. The update hook suppresses only its combat decisions.
			actor->InitiateDoNothingPackage();
			a_npc.ready = true;
			logs::info(
				"Remote-puppet actor became ready: actor={:08X} base={:08X} desired={} hasCollision={} aiEnabled={} waitedFrames={}",
				a_npc.handle.native_handle(),
				actor->GetActorBase() ? actor->GetActorBase()->GetFormID() : 0,
				Multiplayer::GetMovementStateName(
					a_npc.targetAnimationState.movementState),
				actor->HasCollision(),
				actor->IsAIEnabled(),
				a_npc.initializationWaitFrames);
		}
		EnablePuppetAnimationFirewall(a_npc);
		const bool furnitureLifecycleActive =
			a_npc.requestedFurnitureRefID != 0 ||
			(a_npc.animationState &&
				a_npc.animationState->furnitureAnimationOwned);
		if (furnitureLifecycleActive &&
			AdvanceFurnitureInteraction(*actor, a_npc, a_deltaTime)) {
			// Once native entry starts, furniture owns placement, heading, and the
			// graph. In particular, do not replay locomotion during graph recovery.
			return true;
		}

		ApplyAnimationUpdate(*actor, a_npc, a_deltaTime);
		const bool landingCompletionPending = a_npc.animationState &&
			a_npc.animationState->landingCompletionPending;
		if (!IsAnimationStateSettled(a_npc) && !landingCompletionPending) {
			// Direct transform motion while the graph is unavailable is the primary
			// visible sliding failure. Keep the latest target, but do not move until
			// its matching locomotion revision has been accepted or bounded out.
			return true;
		}
		if (!furnitureLifecycleActive &&
			AdvanceFurnitureInteraction(*actor, a_npc, a_deltaTime)) {
			// Native furniture controls placement, heading, and the graph. Applying
			// network transform interpolation here would pull the actor off its marker.
			return true;
		}
		if (!IsAnimationStateSettled(a_npc)) {
			// A completed furniture exit invalidates the old locomotion dispatch so
			// the next update can reconstruct it before translation resumes.
			return true;
		}
		if (!a_allowTranslation && !landingCompletionPending) {
			// A compound transition is still converging toward the latest locomotion
			// state. Applying its transform during an intermediate state would pair
			// position from the latest sample with stale graph state.
			return true;
		}

		const float deltaTime = std::clamp(a_deltaTime, 0.0F, kMaximumFrameTime);
		ApplySmoothedHeading(*actor, a_npc.targetHeading, deltaTime);

		const auto current = actor->GetPosition();
		const auto& target = a_npc.targetPosition;

		const RE::NiPoint3 offset{
			target.x - current.x,
			target.y - current.y,
			target.z - current.z
		};
		const float distanceSquared =
			offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;

		const float distance = std::sqrt(distanceSquared);
		const float distanceToTravel = distance - a_npc.stoppingDistance;
		if (distanceToTravel <= kArrivalTolerance) {
			return true;
		}
		if (!IsMoving(a_npc.targetAnimationState.movementState) &&
			!a_npc.targetAnimationState.jumping) {
			// An idle snapshot is a current-state checkpoint, not an instruction to
			// visibly walk the remaining interpolation backlog while playing idle.
			actor->SetPosition(target, true);
			return true;
		}

		if (deltaTime <= 0.0F) {
			return true;
		}

		const float interpolationSpeed = a_npc.targetAnimationState.jumping ?
			(std::max)(
				a_npc.translationSpeed,
				distanceToTravel / kAirborneTransformCatchUpTime) :
			a_npc.translationSpeed;
		const float step = (std::min)(
			interpolationSpeed * deltaTime,
			distanceToTravel);
		const float interpolation = step / distance;

		const RE::NiPoint3 nextPosition{
			current.x + offset.x * interpolation,
			current.y + offset.y * interpolation,
			current.z + offset.z * interpolation
		};

		// Passing true keeps the actor's character controller in sync with its
		// reference transform, which is essential for a network-controlled puppet.
		actor->SetPosition(nextPosition, true);
		return true;
	}
}
