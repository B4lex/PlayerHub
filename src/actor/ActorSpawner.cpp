#include "actor/ActorSpawner.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorRegistry.h"
#include "actor/SurvivalPolicy.h"
#include "runtime/GameThread.h"

#include <cmath>
#include <memory>
#include <utility>

namespace ActorSpawner
{
	namespace
	{
		// PlayerBase (00000007) is not an ordinary placeable NPC record. Use a
		// known-good actor base only to make CreateReferenceAtLocation construct the
		// reference, then replace it with PlayerBase before 3D is initialized.
		constexpr RE::FormID kActorConstructionTemplateFormID = 0x00013BBF;
	}

	std::optional<RemoteActor> Spawn(
		RE::TESNPC* a_npcBase,
		const RE::NiPoint3& a_targetPosition,
		float a_targetHeading,
		Multiplayer::AnimationState a_animationState,
		float a_speed,
		float a_stoppingDistance)
	{
		if (!GameThread::Require("SpawnControlledNPC") ||
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
				// is loaded instead of briefly creating the remote actor beside the player.
				return std::nullopt;
			}
		}

		// Remote actors must survive ordinary exterior cell unloads within one worldspace.
		// Interior/worldspace changes are handled as explicit respawn boundaries by
		// SyncCoordinator. Create the reference at the first compatible snapshot so its
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

		auto animationState = std::make_unique<ActorAnimation::State>();
		animationState->targetAnimationState = a_animationState;
		ActorAnimation::RequestAnimationUpdate(animationState.get(), a_animationState, a_speed, 0.0F);
		logs::info(
			"Created remote-puppet actor reference: actor={:08X} base={:08X} constructionBase={:08X} revision={} desired={}/{}",
			actor->GetHandle().native_handle(),
			actor->GetActorBase() ? actor->GetActorBase()->GetFormID() : 0,
			constructionBase->GetFormID(),
			static_cast<unsigned long long>(animationState->desiredRevision.load()),
			Multiplayer::GetMovementStateName(a_animationState.movementState),
			a_animationState.jumping ? "jumping" : "grounded");

		RemoteActor remoteActor{
			.handle = actor->GetHandle(),
			.animationGraphHolder = nullptr,
			.movement = ActorMovement::State{
				.targetPosition = a_targetPosition,
				.targetHeading = a_targetHeading,
				.translationSpeed = a_speed,
				.animationSpeed = a_speed,
				.stoppingDistance = a_stoppingDistance },
			.animation = std::move(animationState),
			.initializationWaitFrames = 0,
			.ready = false
		};
		ActorRegistry::Register(remoteActor, *actor);
		SurvivalPolicy::ApplyRemoteReplicaPolicy(*actor);
		return std::optional<RemoteActor>{ std::move(remoteActor) };
	}

	void Remove(RemoteActor& a_remote)
	{
		if (!GameThread::Require("RemoveControlledNPC")) {
			return;
		}

		ActorRegistry::Unregister(a_remote);
		auto actorPtr = a_remote.handle.get();
		if (!actorPtr) {
			return;
		}

		auto* actor = actorPtr.get();
		actor->Disable();
		actor->SetDelete(true);
	}
}
