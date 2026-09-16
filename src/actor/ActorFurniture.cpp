#include "actor/ActorFurniture.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRegistry.h"
#include "actor/AnimationFirewall.h"
#include "actor/RemoteActor.h"
#include "runtime/GameThread.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace ActorFurniture
{
	namespace
	{
		using ActorConstants::kMaximumFrameTime;

		constexpr float kFurnitureRetryInterval = 1.0F;
		constexpr float kFurnitureEntryFallbackSeconds = 10.0F;
		constexpr float kFurnitureExitFallbackSeconds = 10.0F;
		constexpr float kFurnitureEntryAlignmentDistance = 24.0F;
		// Native entry aligns the actor to the chosen entry point itself; this only
		// bounds how far that alignment may glide the remote actor.
		constexpr float kFurnitureEntryMarkerDistance = 96.0F;

		void SetFurnitureAnimationOwnership(
			RemoteActor& a_remote,
			bool a_owned)
		{
			a_remote.furniture.furnitureAnimationOwned = a_owned;
			ActorRegistry::SetFurnitureAnimationOwned(a_remote, a_owned);
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

		bool UseNativeFurniture(RE::Actor& a_actor, RE::TESObjectREFR& a_furniture)
		{
			// TESFurniture::Activate rejects every activator except the player, so use
			// the entry NPC Sit procedures use (ID 37330). It records the occupied
			// reference, lets ID 39923 choose an entry point (filling furnitureIdle,
			// furniturePathPoint and the entry angle) and reserves that marker.
			// UpdateNativeSitSleepState then plays the chosen entry idle. The quick
			// entry (ID 37324) is not used: it selects the Idle*EnterInstant records
			// and skips the transition.
			using UseFurniture = bool(RE::Actor*, RE::TESObjectREFR*);
			static REL::Relocation<UseFurniture> useFurniture{ REL::ID(37330) };
			return useFurniture(std::addressof(a_actor), std::addressof(a_furniture));
		}

		bool IsFurnitureEntryPointChosen(const RE::AIProcess& a_process)
		{
			// ID 39923 leaves the marker unset when it only requested a sheathe.
			return a_process.middleHigh &&
				a_process.middleHigh->currentFurnitureMarkerID !=
					static_cast<std::uint32_t>(-1);
		}

		bool StartNativeFurnitureEntry(
			RemoteActor& a_remote,
			RE::Actor& a_actor,
			RE::TESObjectREFR& a_furniture,
			float a_markerDistance)
		{
			auto* process = a_actor.GetActorRuntimeData().currentProcess;
			if (!REL::Module::IsAE() || !process || !process->middleHigh) {
				return false;
			}

			(void) SetActorSitSleepState(a_actor, RE::SIT_SLEEP_STATE::kNormal);
			const bool started = UseNativeFurniture(a_actor, a_furniture);
			const auto* entryIdle = process->middleHigh->furnitureIdle;
			if (!started) {
				// Occupancy is recorded before the entry point is chosen and is left
				// behind when none is available.
				process->ClearFurniture();
			}
			const auto* actorState = a_actor.AsActorState();
			logs::info(
				"Animation sync started furniture entry: actor={:08X} furniture={:08X} started={} marker={} markerDistance={:.2f} entryIdle={:08X} weaponState={} inCombat={}",
				a_remote.handle.native_handle(),
				a_furniture.GetFormID(),
				started,
				process->middleHigh->currentFurnitureMarkerID,
				a_markerDistance,
				started && entryIdle ? entryIdle->GetFormID() : 0,
				actorState ?
					static_cast<std::underlying_type_t<RE::WEAPON_STATE>>(
						actorState->GetWeaponState()) :
					0,
				a_actor.IsInCombat());
			return started;
		}

		void UpdateNativeSitSleepState(RE::Actor& a_actor)
		{
			// Actor::UpdateSitSleepState (ID 37332 -> AIProcess ID 39861) advances
			// Normal -> WantToSit -> WaitingForSitAnim and WantToStand -> Normal.
			// Skyrim calls it from package procedure branches that the remote actor's
			// do-nothing package never reaches.
			if (!REL::Module::IsAE()) {
				return;
			}
			using func_t = void(RE::Actor*);
			static REL::Relocation<func_t> func{ REL::ID(37332) };
			func(std::addressof(a_actor));
		}

		void ResetFurnitureRequest(RemoteActor& a_remote)
		{
			a_remote.furniture.requestedFurnitureRefID = 0;
			a_remote.furniture.furnitureRetryRemaining = 0.0F;
			a_remote.furniture.furnitureTransitionElapsed = 0.0F;
		}

		bool AdvanceDeferredFurnitureEntry(
			RE::Actor& a_actor,
			RemoteActor& a_remote,
			RE::AIProcess& a_process,
			RE::SIT_SLEEP_STATE a_sitSleepState,
			float a_deltaTime)
		{
			// With a weapon drawn, ID 39923 requests a sheathe and returns without an
			// entry point, so no entry idle can play. Skyrim's Sit procedure (ID 39648)
			// selects again on later updates while the marker is unset; do the same
			// once the sheathe has finished.
			a_remote.furniture.furnitureTransitionElapsed += a_deltaTime;
			auto furniture = a_actor.GetOccupiedFurniture().get();
			const auto* actorState = a_actor.AsActorState();
			if (furniture && actorState &&
				actorState->GetWeaponState() == RE::WEAPON_STATE::kSheathed &&
				a_remote.furniture.furnitureRetryRemaining <= 0.0F) {
				const bool started = UseNativeFurniture(a_actor, *furniture);
				const bool entryPointChosen = IsFurnitureEntryPointChosen(a_process);
				if (!entryPointChosen) {
					a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryInterval;
				}
				const auto* entryIdle = a_process.middleHigh ?
					a_process.middleHigh->furnitureIdle :
					nullptr;
				logs::info(
					"Animation sync resumed deferred furniture entry: actor={:08X} furniture={:08X} started={} marker={} entryIdle={:08X} sitSleepState={} elapsed={:.3f}",
					a_remote.handle.native_handle(),
					furniture->GetFormID(),
					started,
					a_process.middleHigh ?
						a_process.middleHigh->currentFurnitureMarkerID :
						static_cast<std::uint32_t>(-1),
					entryIdle ? entryIdle->GetFormID() : 0,
					static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(
						a_sitSleepState),
					a_remote.furniture.furnitureTransitionElapsed);
				if (entryPointChosen) {
					a_remote.furniture.furnitureRetryRemaining = 0.0F;
					a_remote.furniture.furnitureTransitionElapsed = 0.0F;
					return true;
				}
			}
			if (a_remote.furniture.furnitureTransitionElapsed < kFurnitureEntryFallbackSeconds) {
				return true;
			}

			logs::warn(
				"Animation sync abandoned deferred furniture entry for retry: actor={:08X} furniture={:08X} sitSleepState={} weaponState={} elapsed={:.3f}",
				a_remote.handle.native_handle(),
				a_remote.furniture.targetFurnitureRefID,
				static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(
					a_sitSleepState),
				actorState ?
					static_cast<std::underlying_type_t<RE::WEAPON_STATE>>(
						actorState->GetWeaponState()) :
					0,
				a_remote.furniture.furnitureTransitionElapsed);
			a_process.ClearFurniture();
			(void) SetActorSitSleepState(a_actor, RE::SIT_SLEEP_STATE::kNormal);
			a_actor.InitiateDoNothingPackage();
			ResetFurnitureRequest(a_remote);
			a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryInterval;
			SetFurnitureAnimationOwnership(a_remote, false);
			return false;
		}
	}

	void SetFurnitureState(
		RemoteActor& a_remote,
		RE::FormID a_furnitureRefID)
	{
		if (!GameThread::Require("SetFurnitureState") ||
			a_remote.furniture.targetFurnitureRefID == a_furnitureRefID) {
			return;
		}

		const auto previousFurnitureRefID = a_remote.furniture.targetFurnitureRefID;
		a_remote.furniture.targetFurnitureRefID = a_furnitureRefID;
		a_remote.furniture.furnitureRetryRemaining = 0.0F;
		if (a_remote.furniture.requestedFurnitureRefID == 0) {
			// An in-progress exit keeps its elapsed time so that rapid target changes
			// cannot postpone the forced-exit fallback indefinitely.
			a_remote.furniture.furnitureTransitionElapsed = 0.0F;
		}
		logs::info(
			"Animation sync updated furniture request: actor={:08X} previous={:08X} desired={:08X}",
			a_remote.handle.native_handle(),
			previousFurnitureRefID,
			a_furnitureRefID);
	}

	RE::FormID GetTargetFurniture(const RemoteActor& a_remote)
	{
		return a_remote.furniture.targetFurnitureRefID;
	}

	bool IsFurnitureInteractionPlaying(const RemoteActor& a_remote)
	{
		return a_remote.furniture.requestedFurnitureRefID != 0 ||
			a_remote.furniture.furnitureAnimationOwned;
	}

	bool AdvanceFurnitureInteraction(
		RE::Actor& a_actor,
		RemoteActor& a_remote,
		float a_deltaTime)
	{
		const auto targetFurnitureRefID = a_remote.furniture.targetFurnitureRefID;
		const auto occupiedFurnitureRefID =
			GetOccupiedFurnitureRefID(a_actor);
		const float deltaTime = std::clamp(
			a_deltaTime,
			0.0F,
			kMaximumFrameTime);
		a_remote.furniture.furnitureRetryRemaining -= deltaTime;
		const auto sitSleepState = GetActorSitSleepState(a_actor);
		if (sitSleepState != a_remote.furniture.loggedSitSleepState) {
			logs::info(
				"Animation sync puppet sit/sleep state changed: actor={:08X} previous={} current={} occupied={:08X} target={:08X}",
				a_remote.handle.native_handle(),
				static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(
					a_remote.furniture.loggedSitSleepState),
				static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(
					sitSleepState),
				occupiedFurnitureRefID,
				targetFurnitureRefID);
			a_remote.furniture.loggedSitSleepState = sitSleepState;
		}
		if (occupiedFurnitureRefID != 0) {
			UpdateNativeSitSleepState(a_actor);
		}

		// Entry, use, and the furniture graph are driven natively once the target
		// is occupied.
		if (targetFurnitureRefID != 0 &&
			occupiedFurnitureRefID == targetFurnitureRefID) {
			SetFurnitureAnimationOwnership(a_remote, true);
			a_remote.furniture.requestedFurnitureRefID = targetFurnitureRefID;
			auto* process = a_actor.GetActorRuntimeData().currentProcess;
			if (process && !IsFurnitureEntryPointChosen(*process) &&
				!IsFurnitureEntryComplete(sitSleepState)) {
				return AdvanceDeferredFurnitureEntry(
					a_actor,
					a_remote,
					*process,
					sitSleepState,
					deltaTime);
			}
			a_remote.furniture.furnitureRetryRemaining = 0.0F;
			a_remote.furniture.furnitureTransitionElapsed = 0.0F;
			return true;
		}

		if (targetFurnitureRefID != 0 &&
			a_remote.furniture.requestedFurnitureRefID == targetFurnitureRefID &&
			occupiedFurnitureRefID == 0) {
			// Skyrim dropped a started entry. Hold ownership for a bounded time
			// before releasing the request for a fresh attempt.
			SetFurnitureAnimationOwnership(a_remote, true);
			a_remote.furniture.furnitureTransitionElapsed += deltaTime;
			if (a_remote.furniture.furnitureTransitionElapsed <
				kFurnitureEntryFallbackSeconds) {
				return true;
			}

			logs::warn(
				"Animation sync abandoned stuck furniture entry for retry: actor={:08X} furniture={:08X} sitSleepState={} elapsed={:.3f}",
				a_remote.handle.native_handle(),
				targetFurnitureRefID,
				static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(
					sitSleepState),
				a_remote.furniture.furnitureTransitionElapsed);
			a_actor.InitiateDoNothingPackage();
			ResetFurnitureRequest(a_remote);
			SetFurnitureAnimationOwnership(a_remote, false);
			return false;
		}

		const bool mustExitCurrentFurniture =
			occupiedFurnitureRefID != 0 ||
			a_remote.furniture.requestedFurnitureRefID != 0;
		const bool furnitureAnimationOwned = a_remote.furniture.furnitureAnimationOwned;
		if (mustExitCurrentFurniture ||
			(targetFurnitureRefID == 0 && furnitureAnimationOwned)) {
			SetFurnitureAnimationOwnership(a_remote, true);
			if (occupiedFurnitureRefID == 0 &&
				GetActorSitSleepState(a_actor) == RE::SIT_SLEEP_STATE::kNormal) {
				const auto exitedFurnitureRefID =
					a_remote.furniture.requestedFurnitureRefID;
				a_actor.InitiateDoNothingPackage();
				ResetFurnitureRequest(a_remote);
				SetFurnitureAnimationOwnership(a_remote, false);
				if (a_remote.animation) {
					// Furniture graphs do not consistently return to the locomotion
					// variables which preceded entry. Reconstruct the current checkpoint.
					ActorAnimation::InvalidateAnimationDispatch(*a_remote.animation);
				}
				const auto* actorState = a_actor.AsActorState();
				if (a_remote.animation->targetAnimationState.weaponDrawn && actorState &&
					actorState->GetWeaponState() == RE::WEAPON_STATE::kSheathed) {
					// Sitting sheathes the weapon, but checkpoints are not staged while
					// furniture owns the graph, so the request still holds the pre-entry
					// drawn state. Dispatching it now would draw on the reconstruction
					// update and then sheathe once the fresh checkpoint is staged.
					auto sheathedState = a_remote.animation->targetAnimationState;
					sheathedState.weaponDrawn = false;
					ActorAnimation::SetAnimationState(a_remote, sheathedState);
				}
				logs::info(
					"Animation sync completed furniture exit: actor={:08X} furniture={:08X} nextFurniture={:08X}",
					a_remote.handle.native_handle(),
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
					a_remote.handle.native_handle(),
					occupiedFurnitureRefID,
					targetFurnitureRefID,
					static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(
						sitSleepState));
				return true;
			}

			a_remote.furniture.furnitureTransitionElapsed += deltaTime;
			if (a_remote.furniture.furnitureRetryRemaining <= 0.0F) {
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
				const bool eventAccepted = AnimationFirewall::NotifyNetworkAnimationGraph(
					a_actor,
					furnitureExitEvent);
				a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryInterval;
				logs::info(
					"Animation sync requested furniture exit: actor={:08X} occupied={:08X} target={:08X} sitSleepState={} stateAccepted={} eventAccepted={}",
					a_remote.handle.native_handle(),
					occupiedFurnitureRefID,
					targetFurnitureRefID,
					static_cast<std::underlying_type_t<RE::SIT_SLEEP_STATE>>(
						sitSleepState),
					stateAccepted,
					eventAccepted);
			}

			if (a_remote.furniture.furnitureTransitionElapsed >=
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
				(void) AnimationFirewall::NotifyNetworkAnimationGraph(
					a_actor,
					defaultStateEvent);
				a_actor.InitiateDoNothingPackage();
				a_remote.furniture.furnitureTransitionElapsed = 0.0F;
				logs::warn(
					"Animation sync forced stuck furniture exit: actor={:08X} occupied={:08X} target={:08X}",
					a_remote.handle.native_handle(),
					occupiedFurnitureRefID,
					targetFurnitureRefID);
			}
			return true;
		}
		if (targetFurnitureRefID == 0) {
			return false;
		}

		// The target is a FormID resolved locally; Skyrim chooses the entry point,
		// idle, and marker for the remote actor.
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
			if (a_remote.furniture.furnitureRetryRemaining <= 0.0F) {
				a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryInterval;
				logs::warn(
					"Animation sync waiting for furniture reference: actor={:08X} furniture={:08X} resolved={} furnitureBase={} loaded3D={} sameSpace={}",
					a_remote.handle.native_handle(),
					targetFurnitureRefID,
					furniture != nullptr,
					isFurniture,
					loaded3D,
					sameCoordinateSpace);
			}
			// The latest transform remains authoritative while the reference is
			// unavailable. Do not strand the remote actor at its pre-interaction position.
			return false;
		}
		if (!a_remote.ready ||
			!ActorAnimation::IsAnimationStateSettled(a_remote) ||
			ActorAnimation::IsFurnitureEntryBlocked(a_remote) ||
			a_remote.furniture.furnitureRetryRemaining > 0.0F) {
			return false;
		}

		// The source reports occupancy when it activates, then travels to the entry
		// point before its entry idle moves it onto the marker. Waiting for the
		// source to settle on the marker delays the remote actor by the whole entry idle.
		// Start once the remote actor follows the source close to the furniture; the
		// native entry aligns it to the chosen entry point.
		const auto actorPosition = a_actor.GetPosition();
		if ((a_remote.movement.targetPosition - actorPosition).Length() >
			kFurnitureEntryAlignmentDistance) {
			return false;
		}
		const auto marker = FindAvailableFurnitureMarker(a_actor, *furniture);
		if (marker < 0) {
			a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryInterval;
			logs::warn(
				"Animation sync found no available furniture marker: actor={:08X} furniture={:08X}",
				a_remote.handle.native_handle(),
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
		a_actor.SetPosition(a_remote.movement.targetPosition, true);
		if (std::isfinite(a_remote.movement.targetHeading)) {
			a_actor.SetHeading(a_remote.movement.targetHeading);
		}
		SetFurnitureAnimationOwnership(a_remote, true);
		a_remote.furniture.furnitureRetryRemaining = kFurnitureRetryInterval;
		if (!StartNativeFurnitureEntry(
				a_remote,
				a_actor,
				*furniture,
				markerDistance)) {
			// Do not freeze translation or leave the graph open to local events.
			SetFurnitureAnimationOwnership(a_remote, false);
			return false;
		}
		a_remote.furniture.requestedFurnitureRefID = targetFurnitureRefID;
		a_remote.furniture.furnitureTransitionElapsed = 0.0F;
		if (auto* process = a_actor.GetActorRuntimeData().currentProcess;
			process && !IsFurnitureEntryPointChosen(*process)) {
			// Entry is waiting for a sheathe; resume as soon as it completes.
			a_remote.furniture.furnitureRetryRemaining = 0.0F;
		}
		return true;
	}
}
