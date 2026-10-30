#include "actor/GraphRecovery.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorRegistry.h"
#include "actor/AnimationFirewall.h"
#include "actor/RemoteActor.h"
#include "runtime/FrameTime.h"
#include "runtime/Profiling.h"

#include <utility>

namespace GraphRecovery
{
    namespace
    {
        constexpr float kHitReactionObservationSeconds = 0.1F;
        // Main-loop passes: the reverted graph and controller stay available across consecutive passes.
        constexpr std::uint32_t kStableFrames = 2;
        constexpr float kFallbackSeconds = 2.0F;
        constexpr std::uint32_t kWaitLogInterval = 120;

        bool RevertAnimationGraphManager(RE::Actor& a_actor)
        {
            using func_t = bool(RE::IAnimationGraphManagerHolder*);
            static REL::Relocation<func_t> func{ RELOCATION_ID(32139, 32883) };
            auto* holder = static_cast<RE::IAnimationGraphManagerHolder*>(std::addressof(a_actor));
            return func(holder);
        }

        bool IsActorInHitReaction(RE::Actor& a_actor)
        {
            static const RE::BSFixedString isRecoilingVariable{ "IsRecoiling" };
            bool isRecoiling = false;
            const bool recoilAvailable = a_actor.GetGraphVariableBool(isRecoilingVariable, isRecoiling);
            const auto* actorState = a_actor.AsActorState();
            return a_actor.IsStaggering() || (recoilAvailable && isRecoiling) || a_actor.IsInRagdollState()
                || (actorState && actorState->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal)
                || a_actor.GetActorRuntimeData().boolBits.all(RE::Actor::BOOL_BITS::kParalyzed);
        }

        void Begin(RE::Actor& a_actor, RemoteActor& a_remote, std::uint32_t a_observedHitEvents)
        {
            auto& state = a_remote.animation.recovery;
            RE::BSAnimationGraphManager* graphBeforeRecovery = nullptr;
            {
                RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
                if (a_actor.GetAnimationGraphManager(graphManager) && graphManager)
                {
                    graphBeforeRecovery = graphManager.get();
                }
            }

            // TESHitEvent can arrive while animation work runs elsewhere.
            AnimationFirewall::Disable(a_remote);
            state.localInterruptionLatched = true;
            state.inProgress = true;
            state.graphBeforeRecovery = graphBeforeRecovery;
            state.waitFrames = 0;
            state.waitElapsed = 0.0F;
            state.stableFrames = 0;
            ActorAnimation::InvalidateAnimationDispatch(a_remote.animation);
            a_remote.ready = false;
            a_remote.initializationWaitFrames = 0;

            // Hit processing can enter the behavior graph below the public event hook
            // and can leave variables, active transitions, and pose state inconsistent
            // with replicated locomotion. Revert the complete graph to its initial
            // state. If the engine cannot do that, rebuilding 3D is the heavier fallback
            // which also reconstructs the graph.
            auto& runtimeData = a_actor.GetActorRuntimeData();
            const auto* actorState = a_actor.AsActorState();
            const bool requiresPhysicalReset = a_actor.IsInRagdollState()
                || (actorState && actorState->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal)
                || runtimeData.boolBits.all(RE::Actor::BOOL_BITS::kParalyzed);
            runtimeData.boolBits.reset(RE::Actor::BOOL_BITS::kParalyzed);
            if (requiresPhysicalReset)
            {
                a_actor.PotentiallyFixRagdollState();
            }
            if (auto* root = a_actor.Get3D())
            {
                root->RemoveDecals();
            }
            const bool graphReverted
                = !requiresPhysicalReset && graphBeforeRecovery && RevertAnimationGraphManager(a_actor);
            state.used3DReset = !graphReverted;
            if (!graphReverted)
            {
                a_actor.DoReset3D(false);
            }
            const auto recoveryCount = ++state.count;
            logs::debug(
                "Started actor animation-graph recovery: actor={:08X} desired={} observedHits={} graphReverted={} "
                "fallback3DReset={} previousGraph={} recovery={}",
                a_remote.handle.native_handle(),
                Multiplayer::GetMovementStateName(a_remote.animation.targetAnimationState.movementState),
                a_observedHitEvents, graphReverted, !graphReverted, static_cast<const void*>(graphBeforeRecovery),
                recoveryCount);
        }
    }

    bool Recover(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime)
    {
        PROFILE_FUNCTION();
        auto& state = a_remote.animation.recovery;
        const auto pendingHitObservations = ActorRegistry::ConsumeHitObservations(a_remote);
        const bool isInterrupted = a_actor.Is3DLoaded() && IsActorInHitReaction(a_actor);

        if (pendingHitObservations != 0)
        {
            state.observedHitEvents += pendingHitObservations;
            state.hitReactionObservationRemaining = kHitReactionObservationSeconds;
        }

        // TESHitEvent is emitted even when the damage firewall successfully makes
        // the hit harmless. Rebuild only after the graph or actor state confirms a
        // real reaction; the short observation window catches reactions that begin
        // a frame or two after their event.
        if (isInterrupted && !state.localInterruptionLatched)
        {
            const auto observedHitEvents = std::exchange(state.observedHitEvents, 0);
            state.hitReactionObservationRemaining = 0.0F;
            Begin(a_actor, a_remote, observedHitEvents);
            return true;
        }

        const float deltaTime = FrameTime::Clamp(a_deltaTime);
        if (state.hitReactionObservationRemaining > 0.0F)
        {
            state.hitReactionObservationRemaining -= deltaTime;
            if (state.hitReactionObservationRemaining <= 0.0F)
            {
                state.hitReactionObservationRemaining = 0.0F;
                state.observedHitEvents = 0;
            }
        }

        if (!state.inProgress)
        {
            if (!isInterrupted)
            {
                state.localInterruptionLatched = false;
            }
            return false;
        }

        ++state.waitFrames;
        state.waitElapsed += deltaTime;
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
        const bool graphReady = a_actor.Is3DLoaded() && a_actor.GetCharController()
            && a_actor.GetAnimationGraphManager(graphManager) && graphManager;
        if (!graphReady)
        {
            state.stableFrames = 0;
            if (!state.used3DReset && state.waitElapsed >= kFallbackSeconds)
            {
                state.used3DReset = true;
                state.waitFrames = 0;
                state.waitElapsed = 0.0F;
                a_actor.DoReset3D(false);
                logs::warn("Actor graph revert did not become ready; escalating to 3D reset: actor={:08X} recovery={}",
                    a_remote.handle.native_handle(), state.count);
                return true;
            }
            if (state.waitFrames == 1 || state.waitFrames % kWaitLogInterval == 0)
            {
                logs::debug(
                    "Waiting for recovered actor animation graph: actor={:08X} loaded3D={} controller={} graph={} "
                    "waitedFrames={} recovery={}",
                    a_remote.handle.native_handle(), a_actor.Is3DLoaded(), a_actor.GetCharController() != nullptr,
                    graphManager != nullptr, state.waitFrames, state.count);
            }
            return true;
        }

        ++state.stableFrames;
        if (state.stableFrames < kStableFrames)
        {
            return true;
        }

        const bool graphWasReplaced = graphManager.get() != state.graphBeforeRecovery;
        const bool used3DReset = state.used3DReset;
        const auto count = state.count;
        // A hit event can arrive just before a shout transitions the actor into
        // ragdoll/paralysis. Clearing the latch lets the next frame start physical
        // recovery if that state appeared after this recovery began.
        state = State{ .observedHitEvents = state.observedHitEvents,
            .hitReactionObservationRemaining = state.hitReactionObservationRemaining,
            .count = count };
        logs::debug(
            "Recovered actor animation graph is stable; replaying authoritative locomotion: actor={:08X} desired={} "
            "graphReplaced={} fallback3DReset={} recovery={}",
            a_remote.handle.native_handle(),
            Multiplayer::GetMovementStateName(a_remote.animation.targetAnimationState.movementState), graphWasReplaced,
            used3DReset, count);
        return false;
    }
}
