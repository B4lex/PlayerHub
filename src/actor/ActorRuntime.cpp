#include "actor/ActorRuntime.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorConstants.h"
#include "actor/ActorFurniture.h"
#include "actor/ActorMovement.h"
#include "actor/ActorRegistry.h"
#include "actor/AnimationFirewall.h"
#include "actor/RemoteActor.h"
#include "actor/SurvivalPolicy.h"
#include "runtime/GameThread.h"

#include <algorithm>

namespace ActorRuntime
{
    namespace
    {
        constexpr std::uint32_t kActorInitializationLogInterval = 120;
        // Saturation point for RemoteActor::readyElapsed. It only has to outlast the
        // longest spawn settling gate, so it never grows without bound.
        constexpr float kReadyElapsedCeilingSeconds = 10.0F;
    }

    bool Update(RemoteActor& a_remote, float a_deltaTime, std::optional<Multiplayer::AnimationState> a_translationState)
    {
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
        if (a_remote.animation)
        {
            ActorAnimation::AdvanceAttackPlayback(*actor, *a_remote.animation, a_deltaTime);
        }
        SurvivalPolicy::ApplyRemoteReplicaPolicy(*actor);
        // Evaluated once per pass, before anything below starts or ends an
        // interaction: the furniture gates below ask about the same pass.
        const bool furnitureLifecycleActive = ActorFurniture::IsFurnitureInteractionPlaying(a_remote);
        // Combat packages can be selected before the suppression flags are
        // reasserted, especially during town-wide alarm/flee evaluation. Mutate
        // package and weapon state only at this established main-loop boundary,
        // never from inside Skyrim's per-actor update hook. Sanitization owns the
        // package once the actor is ready, which installs its first one below.
        // Furniture interactions run on the do-nothing package as well, and a
        // package the engine selects while one runs takes the occupancy back.
        // Installing the do-nothing package does not clear furniture occupancy.
        SurvivalPolicy::SanitizeRemoteCombatState(*actor, a_remote.ready);
        if (ActorAnimation::RecoverAuthoritativeAnimation(*actor, a_remote, a_deltaTime))
        {
            // Keep transform and graph writes paused until the reverted graph and its
            // controller have remained available across consecutive main-loop passes.
            return true;
        }
        auto* controller = actor->GetCharController();

        // Reference creation may return before the actor has a loaded 3D and
        // character controller. Disabling AI at spawn time clears kProcessMe,
        // preventing that initialization and leaving the actor in a T-pose.
        if (!a_remote.ready)
        {
            RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
            const bool has3D = actor->Is3DLoaded();
            const bool hasController = controller != nullptr;
            const bool hasGraph
                = has3D && hasController && actor->GetAnimationGraphManager(graphManager) && graphManager;
            if (!has3D || !hasController || !hasGraph)
            {
                ++a_remote.initializationWaitFrames;
                if (a_remote.initializationWaitFrames == 1
                    || a_remote.initializationWaitFrames % kActorInitializationLogInterval == 0)
                {
                    logs::info(
                        "Waiting for remote actor actor initialization: actor={:08X} base={:08X} loaded3D={} "
                        "controller={} graph={} waitedFrames={}",
                        a_remote.handle.native_handle(), actor->GetActorBase() ? actor->GetActorBase()->GetFormID() : 0,
                        has3D, hasController, hasGraph, a_remote.initializationWaitFrames);
                }
                return true;
            }

            // Keep the actor in the normal update pipeline, but give it no autonomous
            // travel package. The update hook suppresses only its combat decisions.
            SurvivalPolicy::AssertDoNothingPackage(*actor);
            a_remote.ready = true;
            logs::info(
                "Remote actor actor became ready: actor={:08X} base={:08X} desired={} hasCollision={} aiEnabled={} "
                "waitedFrames={}",
                a_remote.handle.native_handle(), actor->GetActorBase() ? actor->GetActorBase()->GetFormID() : 0,
                Multiplayer::GetMovementStateName(a_remote.animation->targetAnimationState.movementState),
                actor->HasCollision(), actor->IsAIEnabled(), a_remote.initializationWaitFrames);
        }
        else if (a_remote.readyElapsed < kReadyElapsedCeilingSeconds)
        {
            // Accumulated here rather than in a feature module: this is the one pass
            // that always runs for a ready actor, whatever a feature does below.
            a_remote.readyElapsed
                = (std::min)(a_remote.readyElapsed + std::clamp(a_deltaTime, 0.0F, ActorConstants::kMaximumFrameTime),
                    kReadyElapsedCeilingSeconds);
        }
        if (actor->Is3DLoaded())
        {
            // Without 3D there is no graph to protect, and ApplyAnimationUpdate
            // disables the firewall again, which would log every pass.
            AnimationFirewall::Enable(a_remote);
        }
        if (furnitureLifecycleActive && ActorFurniture::AdvanceFurnitureInteraction(*actor, a_remote, a_deltaTime))
        {
            // Once native entry starts, furniture owns placement, heading, and the
            // graph. In particular, do not replay locomotion during graph recovery.
            return true;
        }

        // Before ApplyAnimationUpdate, whose events can reactivate an idle turn machine.
        ActorAnimation::HoldIdleTurnState(*actor, a_remote);
        ActorAnimation::ApplyAnimationUpdate(*actor, a_remote, a_deltaTime);
        const bool landingCompletionPending = a_remote.animation && a_remote.animation->landingCompletionPending;
        // Evaluated once per pass: it ages the bounded hold, and both gates below
        // ask the same question about the same pass.
        const bool holdForUnsettledDispatch = ActorAnimation::ShouldHoldTranslation(a_remote, a_deltaTime);
        if (holdForUnsettledDispatch && !landingCompletionPending)
        {
            // Direct transform motion while the graph is unavailable is the primary
            // visible sliding failure. Keep the latest target, but do not move until
            // its matching locomotion revision has been accepted or bounded out.
            return true;
        }
        if (!furnitureLifecycleActive && ActorFurniture::AdvanceFurnitureInteraction(*actor, a_remote, a_deltaTime))
        {
            // Native furniture controls placement, heading, and the graph. Applying
            // network transform interpolation here would pull the actor off its marker.
            return true;
        }
        if (holdForUnsettledDispatch)
        {
            // A completed furniture exit invalidates the old locomotion dispatch so
            // the next update can reconstruct it before translation resumes. The
            // landing branch itself keeps following the grounded network path.
            return true;
        }
        if (!a_translationState && !landingCompletionPending)
        {
            // A compound transition is still converging toward the latest locomotion
            // state. Applying its transform during an intermediate state would pair
            // position from the latest sample with stale graph state.
            return true;
        }

        const auto translationState = a_translationState.value_or(a_remote.animation->targetAnimationState);
        ActorMovement::ApplyTranslation(*actor, a_remote, a_deltaTime, translationState);
        return true;
    }
}
