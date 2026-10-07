#include "actor/ActorSpawner.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorFurniture.h"
#include "actor/ActorRegistry.h"
#include "actor/PrecisionIsolation.h"
#include "actor/SurvivalPolicy.h"
#include "runtime/GameThread.h"

#include <cmath>
#include <memory>
#include <utility>

namespace ActorSpawner
{
    namespace
    {
        RE::TESObjectCELL* GetSpawnCell(RE::PlayerCharacter& a_player, const RE::NiPoint3& a_position)
        {
            if (!a_player.GetWorldspace())
            {
                return a_player.GetParentCell();
            }
            // TES::GetCell only resolves exterior cells that are currently loaded.
            auto* tes = RE::TES::GetSingleton();
            return tes ? tes->GetCell(a_position) : nullptr;
        }

        // TESObjectREFR::CheckSaveGame returns false for a reference flagged as
        // permanently deleted, and the change-form writer then drops its record.
        void SetReferenceExcludedFromSaves(RE::TESObjectREFR& a_reference, bool a_excluded)
        {
            constexpr auto kPermanentlyDeleted = RE::TESForm::InGameFormFlag::kRefPermanentlyDeleted;
            if (a_excluded)
            {
                a_reference.inGameFormFlags.set(kPermanentlyDeleted);
            }
            else
            {
                a_reference.inGameFormFlags.reset(kPermanentlyDeleted);
            }
        }
    }

    std::optional<RemoteActor> Spawn(RE::TESNPC* a_npcBase, const RE::NiPoint3& a_targetPosition, float a_targetHeading,
        Multiplayer::AnimationState a_animationState, float a_speed, float a_stoppingDistance)
    {
        if (!GameThread::Require("SpawnControlledNPC") || !a_npcBase || !std::isfinite(a_targetPosition.x)
            || !std::isfinite(a_targetPosition.y) || !std::isfinite(a_targetPosition.z)
            || !std::isfinite(a_targetHeading) || !std::isfinite(a_speed) || a_speed <= 0.0F
            || !std::isfinite(a_stoppingDistance) || a_stoppingDistance < 0.0F)
        {
            return std::nullopt;
        }

        if (a_npcBase->IsPlayer())
        {
            // CreateReferenceAtLocation returns a reference for PlayerBase, but it
            // never completes normal actor/3D initialization.
            logs::critical("Refusing to spawn a remote actor from PlayerBase");
            return std::nullopt;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!player || !player->GetParentCell() || !dataHandler)
        {
            return std::nullopt;
        }

        auto* worldspace = player->GetWorldspace();
        auto* targetCell = GetSpawnCell(*player, a_targetPosition);
        if (!targetCell)
        {
            // Wait until the exterior cell is loaded instead of briefly creating the actor
            // beside the player.
            return std::nullopt;
        }

        const RE::NiPoint3 targetRotation{ 0.0F, 0.0F, a_targetHeading };
        const auto spawnedHandle = dataHandler->CreateReferenceAtLocation(a_npcBase, a_targetPosition, targetRotation,
            targetCell, worldspace, nullptr, nullptr, RE::ObjectRefHandle{}, true, true);
        auto spawnedRef = spawnedHandle.get();
        if (!spawnedRef)
        {
            return std::nullopt;
        }

        auto* actor = spawnedRef->As<RE::Actor>();
        if (!actor)
        {
            return std::nullopt;
        }

        actor->SetActivationBlocked(true);
        // The animation graph and character controller only finish initializing
        // while the actor remains in Skyrim's normal processing pipeline.
        actor->EnableAI(true);

        auto animationState = std::make_unique<ActorAnimation::State>();
        animationState->targetAnimationState = a_animationState;
        ActorAnimation::RequestAnimationUpdate(animationState.get(), a_animationState, a_speed, 0.0F);
        logs::debug("Created remote actor reference: actor={:08X} base={:08X} revision={} desired={}/{}",
            actor->GetHandle().native_handle(), actor->GetActorBase() ? actor->GetActorBase()->GetFormID() : 0,
            static_cast<unsigned long long>(animationState->desiredRevision.load()),
            Multiplayer::GetMovementStateName(a_animationState.movementState),
            a_animationState.jumping ? "jumping" : "grounded");

        RemoteActor remoteActor{ .handle = actor->GetHandle(),
            .animationGraphHolder = nullptr,
            .movement = ActorMovement::State{ .targetPosition = a_targetPosition,
                .checkpointPosition = a_targetPosition,
                .targetHeading = a_targetHeading,
                .translationSpeed = a_speed,
                .animationSpeed = a_speed,
                .stoppingDistance = a_stoppingDistance },
            .animation = std::move(animationState),
            .initializationWaitFrames = 0,
            .ready = false };
        ActorRegistry::Register(remoteActor, *actor);
        PrecisionIsolation::Exclude(remoteActor.handle);
        SurvivalPolicy::ApplyRemoteReplicaPolicy(*actor);
        return std::optional<RemoteActor>{ std::move(remoteActor) };
    }

    void Remove(RemoteActor& a_remote)
    {
        if (!GameThread::Require("RemoveControlledNPC"))
        {
            return;
        }

        auto actorPtr = a_remote.handle.get();
        if (actorPtr)
        {
            ActorFurniture::ReleaseFurniture(*actorPtr, a_remote);
        }
        ActorRegistry::Unregister(a_remote);
        PrecisionIsolation::Release(a_remote.handle);
        if (!actorPtr)
        {
            return;
        }

        auto* actor = actorPtr.get();
        actor->Disable();
        actor->SetDelete(true);
        // Deleting a reference does not retire the change form the save writer
        // keeps for it, so a retired actor is still written into every later save
        // of that playthrough. Such a record carries the runtime actor base the
        // reference was spawned from, and that base is never serialized: on load
        // the record binds to whichever dynamic form receives the same ID while
        // the save is replayed, and the load crashes.
        SetReferenceExcludedFromSaves(*actor, true);
    }

    bool IsSpawnLocationLoaded(const RE::NiPoint3& a_position)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        return player && player->GetParentCell() && GetSpawnCell(*player, a_position) != nullptr;
    }

    void SetExcludedFromSaves(const RemoteActor& a_remote, bool a_excluded)
    {
        if (!GameThread::Require("SetRemoteActorExcludedFromSaves"))
        {
            return;
        }

        auto actorPtr = a_remote.handle.get();
        if (!actorPtr)
        {
            return;
        }

        SetReferenceExcludedFromSaves(*actorPtr, a_excluded);
    }
}
