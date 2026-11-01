#include "actor/ActorRuntime.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorFurniture.h"
#include "actor/ActorMount.h"
#include "actor/ActorMovement.h"
#include "actor/ActorRegistry.h"
#include "actor/AnimationFirewall.h"
#include "actor/AttackDispatch.h"
#include "actor/GraphRecovery.h"
#include "actor/RemoteActor.h"
#include "actor/RemoteCollision.h"
#include "actor/SurvivalPolicy.h"
#include "actor/WeaponTransition.h"
#include "runtime/FrameTime.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"

#include <algorithm>

namespace ActorRuntime
{
    namespace
    {
        constexpr std::uint32_t kActorInitializationLogInterval = 120;
        constexpr float kReadyElapsedCeilingSeconds = 10.0F;

        // Reference creation may return before the actor has a loaded 3D and
        // character controller. Disabling AI at spawn time clears kProcessMe,
        // preventing that initialization and leaving the actor in a T-pose.
        bool AdvanceReadiness(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime)
        {
            if (a_remote.ready)
            {
                if (a_remote.readyElapsed < kReadyElapsedCeilingSeconds)
                {
                    // Accumulated here: this is the one pass that always runs for a ready
                    // actor, whatever a feature does.
                    a_remote.readyElapsed = (std::min)(a_remote.readyElapsed + FrameTime::Clamp(a_deltaTime),
                        kReadyElapsedCeilingSeconds);
                }
                return true;
            }

            RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
            const bool has3D = a_actor.Is3DLoaded();
            const bool hasController = a_actor.GetCharController() != nullptr;
            const bool hasGraph
                = has3D && hasController && a_actor.GetAnimationGraphManager(graphManager) && graphManager;
            if (!has3D || !hasController || !hasGraph)
            {
                ++a_remote.initializationWaitFrames;
                if (a_remote.initializationWaitFrames == 1
                    || a_remote.initializationWaitFrames % kActorInitializationLogInterval == 0)
                {
                    logs::debug(
                        "Waiting for remote actor actor initialization: actor={:08X} base={:08X} loaded3D={} "
                        "controller={} graph={} waitedFrames={}",
                        a_remote.handle.native_handle(),
                        a_actor.GetActorBase() ? a_actor.GetActorBase()->GetFormID() : 0, has3D, hasController,
                        hasGraph, a_remote.initializationWaitFrames);
                }
                return false;
            }

            SurvivalPolicy::AssertDoNothingPackage(a_actor);
            a_remote.ready = true;
            logs::debug(
                "Remote actor actor became ready: actor={:08X} base={:08X} desired={} hasCollision={} aiEnabled={} "
                "waitedFrames={}",
                a_remote.handle.native_handle(), a_actor.GetActorBase() ? a_actor.GetActorBase()->GetFormID() : 0,
                Multiplayer::GetMovementStateName(a_remote.animation.targetAnimationState.movementState),
                a_actor.HasCollision(), a_actor.IsAIEnabled(), a_remote.initializationWaitFrames);
            return true;
        }
    }

    bool Update(RemoteActor& a_remote, float a_deltaTime, std::optional<Multiplayer::AnimationState> a_presentableState)
    {
        PROFILE_FUNCTION();
        if (!GameThread::Require("UpdateControlledNPC"))
        {
            return true;
        }

        auto actorPtr = a_remote.handle.get();
        if (!actorPtr)
        {
            ActorRegistry::Unregister(a_remote);
            return false;
        }

        auto* actor = actorPtr.get();
        AttackDispatch::Advance(*actor, a_remote, a_deltaTime);
        SurvivalPolicy::ApplyRemoteReplicaPolicy(*actor);
        const bool furnitureLifecycleActive = ActorFurniture::IsFurnitureInteractionPlaying(a_remote);
        // Combat packages can be selected before the suppression flags are
        // reasserted, especially during town-wide alarm/flee evaluation. A package
        // the engine selects during a furniture interaction takes the occupancy back.
        SurvivalPolicy::SanitizeRemoteCombatState(*actor, a_remote.ready);
        if (GraphRecovery::Recover(*actor, a_remote, a_deltaTime))
        {
            // Keep transform and graph writes paused until the reverted graph and its
            // controller stay available across consecutive passes.
            return true;
        }
        if (!AdvanceReadiness(*actor, a_remote, a_deltaTime))
        {
            return true;
        }
        if (actor->Is3DLoaded())
        {
            // Without 3D there is no graph to protect, and ApplyAnimationUpdate disables the
            // firewall again, which would log every pass.
            AnimationFirewall::Enable(a_remote);
        }
        if (furnitureLifecycleActive && ActorFurniture::AdvanceFurnitureInteraction(*actor, a_remote, a_deltaTime))
        {
            // Once native entry starts, furniture owns placement, heading, and the graph; do
            // not replay locomotion during graph recovery.
            RemoteCollision::ReleaseToEngine(*actor, a_remote, "furniture", false);
            return true;
        }
        RemoteCollision::PlaceByNetwork(*actor, a_remote);

        // Graph events can reactivate an idle turn machine.
        ActorAnimation::HoldIdleTurnState(*actor, a_remote);
        ActorAnimation::ApplyAnimationUpdate(*actor, a_remote, a_deltaTime);
        if (!furnitureLifecycleActive && ActorFurniture::AdvanceFurnitureInteraction(*actor, a_remote, a_deltaTime))
        {
            // Network interpolation here would pull the actor off its furniture marker.
            RemoteCollision::ReleaseToEngine(*actor, a_remote, "furniture", false);
            return true;
        }
        auto mode = ActorAnimation::GetTranslationMode(a_remote, a_presentableState, a_deltaTime);
        if (a_remote.movement.poseWeight > 0.0F)
        {
            // The graph underneath may lag the network state, but the pose does not.
            mode = ActorMovement::TranslationMode::kPosed;
        }
        if (mode)
        {
            ActorMovement::ApplyTranslation(*actor, a_remote, a_deltaTime, *mode);
        }
        return true;
    }

    bool UpdateRider(
        RemoteActor& a_remote, ActorMount::Gait& a_gait, float a_mountSpeed, bool a_weaponDrawn, float a_deltaTime)
    {
        PROFILE_FUNCTION();
        if (!GameThread::Require("UpdateRemoteRider"))
        {
            return true;
        }

        auto actorPtr = a_remote.handle.get();
        if (!actorPtr)
        {
            ActorRegistry::Unregister(a_remote);
            return false;
        }

        auto* actor = actorPtr.get();
        SurvivalPolicy::ApplyRemoteReplicaPolicy(*actor);
        SurvivalPolicy::SanitizeRemoteCombatState(*actor, a_remote.ready);
        // Skyrim carries a seated rider with its mount.
        const bool carried = actor->IsOnMount();
        RemoteCollision::ReleaseToEngine(*actor, a_remote, "mount", carried);
        if (carried)
        {
            RemoteCollision::IsolateCarriedSkeleton(*actor, a_remote, a_deltaTime);
        }
        if (AdvanceReadiness(*actor, a_remote, a_deltaTime))
        {
            ActorMount::ApplyGait(*actor, a_remote, a_gait, a_mountSpeed, a_deltaTime, false, "rider");
            WeaponTransition::ApplyRiderWeaponState(*actor, a_remote, a_weaponDrawn);
        }
        return true;
    }

    bool UpdateMount(RemoteActor& a_remote, ActorMount::Gait& a_gait, float a_deltaTime)
    {
        if (!GameThread::Require("UpdateRemoteMount"))
        {
            return true;
        }

        auto actorPtr = a_remote.handle.get();
        if (!actorPtr)
        {
            ActorRegistry::Unregister(a_remote);
            return false;
        }

        auto* actor = actorPtr.get();
        SurvivalPolicy::ApplyRemoteReplicaPolicy(*actor);
        SurvivalPolicy::SanitizeRemoteCombatState(*actor, a_remote.ready);
        if (!AdvanceReadiness(*actor, a_remote, a_deltaTime))
        {
            return true;
        }
        RemoteCollision::PlaceByNetwork(*actor, a_remote);

        // The mount's own graph is never firewalled: only its gait is driven.
        ActorMount::ApplyGait(*actor, a_remote, a_gait, a_remote.movement.groundSpeed, a_deltaTime, true, "mount");
        ActorMovement::ApplyTranslation(*actor, a_remote, a_deltaTime,
            a_gait.moving ? ActorMovement::TranslationMode::kMoving : ActorMovement::TranslationMode::kStanding);
        ActorMovement::ApplyPitch(*actor, a_remote, a_deltaTime);
        return true;
    }
}
