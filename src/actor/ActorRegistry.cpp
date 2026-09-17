#include "actor/ActorRegistry.h"
#include "actor/AnimationEvents.h"
#include "actor/RemoteActor.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace ActorRegistry
{
	namespace
	{
		using AnimationEvents::IsWeaponTransitionEndEvent;
		using Multiplayer::IsMoving;

		struct Registration
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
			std::uint32_t pendingBashExits{ 0 };
			std::uint32_t hitObservationCount{ 0 };
			std::uint32_t combatSanitizationCount{ 0 };
			std::uint32_t jumpLandEndCount{ 0 };
			std::uint32_t weaponTransitionEndCount{ 0 };
			std::uint32_t bashExitCount{ 0 };
			bool authoritativeJumping{ false };
			// Network travel direction while moving, in graph Direction units. The
			// actor's direction channel would otherwise feed the remote actor's own (empty)
			// movement into the graph on every animation update.
			float authoritativeDirection{ 0.0F };
			bool authoritativeWeaponDrawn{ false };
			bool animationFirewallEnabled{ false };
			bool furnitureAnimationOwned{ false };
			bool castAnimationOwned{ false };
			bool landingCompletionObservationAllowed{ false };
			bool observeJumpLandEndEvent{ false };
			bool observeWeaponTransitionEndEvent{ false };
		};

		std::mutex s_mutex;
		std::unordered_map<
			const RE::IAnimationGraphManagerHolder*,
			Registration>
			s_registrations;
	}

	void Register(RemoteActor& a_remote, RE::Actor& a_actor)
	{
		auto* holder = static_cast<RE::IAnimationGraphManagerHolder*>(
			std::addressof(a_actor));
		if (a_remote.animationGraphHolder == holder) {
			return;
		}

		std::scoped_lock lock{ s_mutex };
		if (a_remote.animationGraphHolder) {
			s_registrations.erase(a_remote.animationGraphHolder);
		}
		s_registrations.insert_or_assign(
			holder,
			Registration{
				.actor = std::addressof(a_actor),
				.magicTarget = a_actor.AsMagicTarget(),
				.actorValueOwner = a_actor.AsActorValueOwner(),
				.actorHandle = a_remote.handle.native_handle(),
				.authoritativeJumping =
					a_remote.animation->targetAnimationState.jumping,
				.authoritativeDirection =
					IsMoving(a_remote.animation->targetAnimationState.movementState) ?
						a_remote.movement.animationDirection : 0.0F,
				.authoritativeWeaponDrawn =
					a_remote.animation->targetAnimationState.weaponDrawn,
				.animationFirewallEnabled = false,
				.furnitureAnimationOwned = a_remote.furniture.furnitureAnimationOwned,
				.observeWeaponTransitionEndEvent =
					a_remote.animation->targetAnimationState.weaponDrawn
			});
		a_remote.animationGraphHolder = holder;
	}

	void Unregister(RemoteActor& a_remote)
	{
		if (!a_remote.animationGraphHolder) {
			return;
		}

		std::scoped_lock lock{ s_mutex };
		s_registrations.erase(a_remote.animationGraphHolder);
		a_remote.animationGraphHolder = nullptr;
	}

	// Safe to call from AI worker threads.
	bool IsRegistered(const RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}

		std::scoped_lock lock{ s_mutex };
		return s_registrations.contains(
			static_cast<const RE::IAnimationGraphManagerHolder*>(a_actor));
	}

	bool EnableAnimationFirewall(const RemoteActor& a_remote)
	{
		std::scoped_lock lock{ s_mutex };
		const auto registration = s_registrations.find(a_remote.animationGraphHolder);
		if (registration != s_registrations.end() &&
			!registration->second.animationFirewallEnabled) {
			registration->second.animationFirewallEnabled = true;
			return true;
		}
		return false;
	}

	void DisableAnimationFirewall(const RemoteActor& a_remote)
	{
		std::scoped_lock lock{ s_mutex };
		const auto registration = s_registrations.find(a_remote.animationGraphHolder);
		if (registration != s_registrations.end()) {
			registration->second.animationFirewallEnabled = false;
		}
	}

	void SetFurnitureAnimationOwned(
		const RemoteActor& a_remote,
		bool a_owned)
	{
		std::scoped_lock lock{ s_mutex };
		const auto registration = s_registrations.find(a_remote.animationGraphHolder);
		if (registration != s_registrations.end()) {
			registration->second.furnitureAnimationOwned = a_owned;
		}
	}

	void SetCastAnimationOwned(
		const RemoteActor& a_remote,
		bool a_owned)
	{
		std::scoped_lock lock{ s_mutex };
		const auto registration = s_registrations.find(a_remote.animationGraphHolder);
		if (registration != s_registrations.end()) {
			registration->second.castAnimationOwned = a_owned;
		}
	}

	bool IsCastAnimationOwned(const RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}

		std::scoped_lock lock{ s_mutex };
		const auto registration = s_registrations.find(
			static_cast<const RE::IAnimationGraphManagerHolder*>(a_actor));
		return registration != s_registrations.end() &&
			registration->second.castAnimationOwned;
	}

	std::optional<GraphInput> FindGraphInput(
		const RE::IAnimationGraphManagerHolder* a_holder)
	{
		std::scoped_lock lock{ s_mutex };
		const auto registration = s_registrations.find(a_holder);
		if (registration == s_registrations.end()) {
			return std::nullopt;
		}
		return GraphInput{
			.actorHandle = registration->second.actorHandle,
			.animationFirewallEnabled = registration->second.animationFirewallEnabled,
			.furnitureAnimationOwned = registration->second.furnitureAnimationOwned,
			.castAnimationOwned = registration->second.castAnimationOwned
		};
	}

	void RecordSwimStop(const RE::IAnimationGraphManagerHolder* a_holder)
	{
		std::scoped_lock lock{ s_mutex };
		const auto registration = s_registrations.find(a_holder);
		if (registration != s_registrations.end()) {
			++registration->second.pendingSwimStops;
		}
	}

	std::uint32_t RecordBlockedGraphEvent(
		const RE::IAnimationGraphManagerHolder* a_holder,
		const char* a_eventName)
	{
		std::scoped_lock lock{ s_mutex };
		const auto registration = s_registrations.find(a_holder);
		if (registration == s_registrations.end()) {
			return 0;
		}

		auto& count = registration->second.blockedEventCounts[
			a_eventName ? a_eventName : "<null>"];
		const auto blockedEventCount = ++count;
		if (a_eventName &&
			std::string_view{ a_eventName } == "IdleCowering") {
			++registration->second.pendingCombatSanitizations;
		}
		return blockedEventCount;
	}

	GraphOutputObservation ObserveGraphOutputEvent(
		const RE::TESObjectREFR* a_holder,
		std::string_view a_eventName,
		bool a_isJumpLandEnd,
		bool a_shouldTraceWeaponEvent)
	{
		GraphOutputObservation observation;
		std::scoped_lock lock{ s_mutex };
		const auto registration = std::ranges::find_if(
			s_registrations,
			[a_holder](const auto& a_entry) {
				return a_entry.second.actor == a_holder;
			});
		if (registration != s_registrations.end() && a_isJumpLandEnd &&
			registration->second.landingCompletionObservationAllowed &&
			registration->second.observeJumpLandEndEvent) {
			++registration->second.pendingJumpLandEnds;
			observation.landingCompletionCount =
				++registration->second.jumpLandEndCount;
			registration->second.observeJumpLandEndEvent = false;
			observation.actorHandle = registration->second.actorHandle;
		}
		if (registration != s_registrations.end() &&
			registration->second.observeWeaponTransitionEndEvent &&
			a_shouldTraceWeaponEvent) {
			observation.traceWeaponEvent = true;
			observation.expectedWeaponDrawn =
				registration->second.authoritativeWeaponDrawn;
			observation.actorHandle = registration->second.actorHandle;
		}
		if (registration != s_registrations.end() &&
			registration->second.observeWeaponTransitionEndEvent &&
			IsWeaponTransitionEndEvent(
				a_eventName,
				registration->second.authoritativeWeaponDrawn)) {
			++registration->second.pendingWeaponTransitionEnds;
			observation.weaponCompletionCount =
				++registration->second.weaponTransitionEndCount;
			registration->second.observeWeaponTransitionEndEvent = false;
			observation.actorHandle = registration->second.actorHandle;
		}
		if (registration != s_registrations.end() &&
			AnimationEvents::EqualsIgnoringCase(a_eventName, "bashExit")) {
			++registration->second.pendingBashExits;
			observation.bashExitCount = ++registration->second.bashExitCount;
			observation.actorHandle = registration->second.actorHandle;
		}
		return observation;
	}

	std::optional<bool> GetAuthoritativeJumpState(const RE::Actor* a_actor)
	{
		if (!a_actor) {
			return std::nullopt;
		}

		std::scoped_lock lock{ s_mutex };
		const auto registration = std::ranges::find_if(
			s_registrations,
			[a_actor](const auto& a_entry) {
				return a_entry.second.actor == a_actor;
			});
		if (registration == s_registrations.end()) {
			return std::nullopt;
		}

		return registration->second.authoritativeJumping;
	}

	void SetAuthoritativeJumpState(
		const RemoteActor& a_remote,
		bool a_jumping)
	{
		std::scoped_lock lock{ s_mutex };
		const auto registration = s_registrations.find(a_remote.animationGraphHolder);
		if (registration != s_registrations.end()) {
			registration->second.authoritativeJumping = a_jumping;
		}
	}

	void SetAuthoritativeDirection(const RemoteActor& a_remote)
	{
		const float direction =
			IsMoving(a_remote.animation->targetAnimationState.movementState) ?
				a_remote.movement.animationDirection : 0.0F;
		std::scoped_lock lock{ s_mutex };
		const auto registration = s_registrations.find(a_remote.animationGraphHolder);
		if (registration != s_registrations.end()) {
			registration->second.authoritativeDirection = direction;
		}
	}

	std::optional<float> GetAuthoritativeDirection(const RE::Actor* a_actor)
	{
		if (!a_actor) {
			return std::nullopt;
		}

		std::scoped_lock lock{ s_mutex };
		const auto registration = std::ranges::find_if(
			s_registrations,
			[a_actor](const auto& a_entry) {
				return a_entry.second.actor == a_actor;
			});
		return registration != s_registrations.end() ?
			std::optional<float>{ registration->second.authoritativeDirection } :
			std::nullopt;
	}

	void SetAuthoritativeWeaponDrawnState(
		const RemoteActor& a_remote,
		bool a_weaponDrawn)
	{
		std::scoped_lock lock{ s_mutex };
		const auto registration = s_registrations.find(a_remote.animationGraphHolder);
		if (registration != s_registrations.end() &&
			registration->second.authoritativeWeaponDrawn != a_weaponDrawn) {
			registration->second.authoritativeWeaponDrawn = a_weaponDrawn;
			// Arm before DrawWeaponMagicHands runs on the next remote actor update. Its
			// completion annotation may be emitted from an animation worker.
			registration->second.pendingWeaponTransitionEnds = 0;
			registration->second.observeWeaponTransitionEndEvent = true;
		}
	}

	std::optional<bool> GetAuthoritativeWeaponDrawnState(
		const RE::Actor& a_actor)
	{
		std::scoped_lock lock{ s_mutex };
		const auto registration = std::ranges::find_if(
			s_registrations,
			[&a_actor](const auto& a_entry) {
				return a_entry.second.actor == std::addressof(a_actor);
			});
		return registration != s_registrations.end() ?
			std::optional<bool>{ registration->second.authoritativeWeaponDrawn } :
			std::nullopt;
	}

	std::pair<std::uint32_t, std::uint32_t> BlockMagicEffect(
		const RE::MagicTarget* a_target)
	{
		std::scoped_lock lock{ s_mutex };
		const auto registration = std::ranges::find_if(
			s_registrations,
			[a_target](const auto& a_entry) {
				return a_entry.second.magicTarget == a_target;
			});
		if (registration == s_registrations.end()) {
			return {};
		}

		return {
			registration->second.actorHandle,
			++registration->second.blockedMagicEffects
		};
	}

	std::pair<std::uint32_t, std::uint32_t> BlockHealthDamage(
		const RE::Actor* a_actor)
	{
		if (!a_actor) {
			return {};
		}

		std::scoped_lock lock{ s_mutex };
		const auto registration = std::ranges::find_if(
			s_registrations,
			[a_actor](const auto& a_entry) {
				return a_entry.second.actor == a_actor;
			});
		if (registration == s_registrations.end()) {
			return {};
		}

		return {
			registration->second.actorHandle,
			++registration->second.blockedHealthDamage
		};
	}

	std::pair<std::uint32_t, std::uint32_t> BlockHealthActorValueDamage(
		const RE::ActorValueOwner* a_actorValueOwner)
	{
		if (!a_actorValueOwner) {
			return {};
		}

		std::scoped_lock lock{ s_mutex };
		const auto registration = std::ranges::find_if(
			s_registrations,
			[a_actorValueOwner](const auto& a_entry) {
				return a_entry.second.actorValueOwner == a_actorValueOwner;
			});
		if (registration == s_registrations.end()) {
			return {};
		}

		return {
			registration->second.actorHandle,
			++registration->second.blockedHealthDamage
		};
	}

	std::pair<std::uint32_t, std::uint32_t> RequestHitObservation(
		const RE::Actor* a_actor)
	{
		if (!a_actor) {
			return {};
		}

		std::scoped_lock lock{ s_mutex };
		const auto registration = std::ranges::find_if(
			s_registrations,
			[a_actor](const auto& a_entry) {
				return a_entry.second.actor == a_actor;
			});
		if (registration == s_registrations.end()) {
			return {};
		}

		++registration->second.pendingHitObservations;
		return {
			registration->second.actorHandle,
			++registration->second.hitObservationCount
		};
	}

	std::uint32_t ConsumeHitObservations(const RemoteActor& a_remote)
	{
		std::scoped_lock lock{ s_mutex };
		const auto registration = s_registrations.find(a_remote.animationGraphHolder);
		if (registration == s_registrations.end()) {
			return 0;
		}

		return std::exchange(registration->second.pendingHitObservations, 0);
	}

	std::uint32_t ConsumeSwimStops(const RemoteActor& a_remote)
	{
		std::scoped_lock lock{ s_mutex };
		const auto registration = s_registrations.find(a_remote.animationGraphHolder);
		if (registration == s_registrations.end()) {
			return 0;
		}

		return std::exchange(registration->second.pendingSwimStops, 0);
	}

	std::uint32_t ConsumeWeaponTransitionEnds(const RemoteActor& a_remote)
	{
		std::scoped_lock lock{ s_mutex };
		const auto registration = s_registrations.find(a_remote.animationGraphHolder);
		if (registration == s_registrations.end()) {
			return 0;
		}

		return std::exchange(
			registration->second.pendingWeaponTransitionEnds,
			0);
	}

	void SetLandingCompletionObservation(
		const RE::Actor& a_actor,
		bool a_allowed)
	{
		std::scoped_lock lock{ s_mutex };
		const auto registration = std::ranges::find_if(
			s_registrations,
			[&a_actor](const auto& a_entry) {
				return a_entry.second.actor == std::addressof(a_actor);
			});
		if (registration != s_registrations.end()) {
			registration->second.landingCompletionObservationAllowed = a_allowed;
			registration->second.observeJumpLandEndEvent = false;
			registration->second.pendingJumpLandEnds = 0;
		}
	}

	void ArmJumpLandEndObservation(const RE::Actor& a_actor)
	{
		std::scoped_lock lock{ s_mutex };
		const auto registration = std::ranges::find_if(
			s_registrations,
			[&a_actor](const auto& a_entry) {
				return a_entry.second.actor == std::addressof(a_actor);
			});
		if (registration != s_registrations.end() &&
			registration->second.landingCompletionObservationAllowed) {
			// Arm only after the grounded transform and graph variables are in
			// place. A delayed marker from the prior jump must not complete this one.
			registration->second.pendingJumpLandEnds = 0;
			registration->second.observeJumpLandEndEvent = true;
		}
	}

	std::uint32_t ConsumeJumpLandEnds(const RemoteActor& a_remote)
	{
		std::scoped_lock lock{ s_mutex };
		const auto registration = s_registrations.find(a_remote.animationGraphHolder);
		if (registration == s_registrations.end()) {
			return 0;
		}

		return std::exchange(registration->second.pendingJumpLandEnds, 0);
	}

	std::uint32_t ConsumeBashExits(const RE::Actor& a_actor)
	{
		std::scoped_lock lock{ s_mutex };
		const auto registration = std::ranges::find_if(
			s_registrations,
			[&a_actor](const auto& a_entry) {
				return a_entry.second.actor == std::addressof(a_actor);
			});
		if (registration == s_registrations.end()) {
			return 0;
		}

		return std::exchange(registration->second.pendingBashExits, 0);
	}

	std::uint32_t ConsumeCombatSanitizationRequests(const RE::Actor& a_actor)
	{
		std::scoped_lock lock{ s_mutex };
		const auto registration = std::ranges::find_if(
			s_registrations,
			[&a_actor](const auto& a_entry) {
				return a_entry.second.actor == std::addressof(a_actor);
			});
		if (registration == s_registrations.end()) {
			return 0;
		}

		return std::exchange(
			registration->second.pendingCombatSanitizations,
			0);
	}

	std::pair<std::uint32_t, std::uint32_t> RecordCombatSanitization(
		const RE::Actor& a_actor)
	{
		std::scoped_lock lock{ s_mutex };
		const auto registration = std::ranges::find_if(
			s_registrations,
			[&a_actor](const auto& a_entry) {
				return a_entry.second.actor == std::addressof(a_actor);
			});
		if (registration == s_registrations.end()) {
			return {};
		}

		return {
			registration->second.actorHandle,
			++registration->second.combatSanitizationCount
		};
	}
}
