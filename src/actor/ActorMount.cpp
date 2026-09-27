#include "actor/ActorMount.h"
#include "actor/ActorAnimation.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRegistry.h"
#include "actor/AnimationFirewall.h"
#include "actor/RemoteActor.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace ActorMount
{
    namespace
    {
        using ActorConstants::kBlockedEventLogInterval;
        using ActorConstants::kInitialBlockedEventLogs;

        // Skyrim.esm EncHorseSaddledBrown: a generic, non-unique saddled horse.
        constexpr RE::FormID kMountBaseID = 0x023AB2;
        // Below the 125 walk speed of Horse_Default_MT.
        constexpr float kMovingSpeed = 20.0F;
        // Between the 450 run of Horse_Default_MT and the 600 of Horse_Sprint_MT,
        // apart so that a measured speed around either does not toggle the gallop.
        constexpr float kGallopStartSpeed = 560.0F;
        constexpr float kGallopStopSpeed = 490.0F;
        constexpr float kGaitDiagnosticSeconds = 2.0F;
        // How often a mount's graph is checked against the requested gait.
        constexpr float kReconcileSeconds = 0.25F;
        // How long the speed must stay in the gallop range before galloping.
        constexpr float kGallopHoldSeconds = 0.3F;

        // The mount engine calls below are Address Library IDs verified only
        // against 1.7.104. Other runtimes never seat a rider.

        // Skyrim's mount entry (ID 38704) builds this interaction and starts it,
        // but only its animated branch builds one; its quick branch, used by
        // PutActorOnMountQuick, restarts the rider's existing interaction. So a new
        // interaction is built the same way, and started quick unless animated.
        // The animated start performs the mount action on the rider's graph, and
        // stops the interaction at once if the graph rejects it.
        bool StartMountInteraction(RE::Actor& a_rider, RE::Actor& a_mount, bool a_animated)
        {
            // MountInteraction(rider, mount), 0x38 bytes from Skyrim's heap. Its
            // reference count starts at zero; the last release destroys and frees it.
            using ConstructMountInteraction = RE::RefrInteraction*(void*, RE::Actor*, RE::Actor*);
            // Records the interaction on the rider and starts it. A quick start sets
            // the process flag that skips the mount animation.
            using StartInteraction = bool(RE::Actor*, RE::BSTSmartPointer<RE::RefrInteraction>*, bool);
            static REL::Relocation<ConstructMountInteraction> construct{ REL::ID(40345) };
            static REL::Relocation<StartInteraction> start{ REL::ID(41530) };
            constexpr std::size_t kMountInteractionSize = 0x38;

            auto* memory = RE::malloc(kMountInteractionSize);
            if (!memory)
            {
                return false;
            }
            RE::BSTSmartPointer<RE::RefrInteraction> interaction{ construct(
                memory, std::addressof(a_rider), std::addressof(a_mount)) };
            return start(std::addressof(a_rider), std::addressof(interaction), !a_animated);
        }

        bool IsGettingOnOrOff(const RE::Actor& a_actor)
        {
            return a_actor.GetActorRuntimeData().boolFlags.any(RE::Actor::BOOL_FLAGS::kGettingOnOffMount);
        }

        // The rider graph's HorseEnter and HorseExit set its sit state, which only
        // returns to riding or normal once the clip ends.
        bool IsRiderClipPlaying(const RE::Actor& a_rider)
        {
            if (!a_rider.IsOnMount())
            {
                return false;
            }
            // Actor's ActorState base moved on AE; AsActorState relocates it.
            const auto* actorState = a_rider.AsActorState();
            if (!actorState)
            {
                return false;
            }
            switch (actorState->GetSitSleepState())
            {
                case RE::SIT_SLEEP_STATE::kWantToSit:
                case RE::SIT_SLEEP_STATE::kWaitingForSitAnim:
                case RE::SIT_SLEEP_STATE::kWantToStand:
                    return true;
                default:
                    return false;
            }
        }

        bool SendGaitEvent(RE::Actor& a_actor, Gait& a_gait, const char* a_eventName, const char* a_role)
        {
            const RE::BSFixedString eventName{ a_eventName };
            const bool accepted = AnimationFirewall::NotifyNetworkAnimationGraph(a_actor, eventName);
            if (!accepted)
            {
                // Reconciliation retries it while the graph still differs.
                const auto rejectedEvents = ++a_gait.rejectedEvents;
                if (rejectedEvents <= kInitialBlockedEventLogs || rejectedEvents % kBlockedEventLogInterval == 0)
                {
                    logs::debug("Remote {} graph rejected gait event: actor={:08X} event={} speed={:.1f} count={}",
                        a_role, a_actor.GetHandle().native_handle(), a_eventName, a_gait.graphSpeed, rejectedEvents);
                }
            }
            return accepted;
        }

        float GetGraphFloat(RE::Actor& a_actor, const RE::BSFixedString& a_variable)
        {
            float value = -1.0F;
            return a_actor.GetGraphVariableFloat(a_variable, value) ? value : -1.0F;
        }
    }

    RE::TESNPC* GetMountBase()
    {
        static RE::TESNPC* s_mountBase = nullptr;
        static bool s_resolved = false;
        if (!s_resolved)
        {
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            s_mountBase = dataHandler ? dataHandler->LookupForm<RE::TESNPC>(kMountBaseID, "Skyrim.esm") : nullptr;
            s_resolved = dataHandler != nullptr;
            if (s_resolved && !s_mountBase)
            {
                logs::error(
                    "Remote mount base Skyrim.esm:{:06X} is unavailable; remote players ride nothing", kMountBaseID);
            }
        }
        return s_mountBase;
    }

    bool Seat(RemoteActor& a_rider, const RemoteActor& a_mount, bool a_animated)
    {
        if (!GameThread::Require("SeatRemoteRider") || !REL::Module::IsAE())
        {
            return false;
        }

        auto riderPtr = a_rider.handle.get();
        auto mountPtr = a_mount.handle.get();
        if (!riderPtr || !mountPtr)
        {
            return false;
        }

        // Before the start, which sends the riding events into the rider's graph.
        ActorRegistry::SetMounted(a_rider, true);
        // The engine update reasserts this, and an on-foot jump must not carry over.
        ActorRegistry::SetAuthoritativeJumpState(a_rider, false);
        const bool started = StartMountInteraction(*riderPtr, *mountPtr, a_animated);
        if (!started)
        {
            ActorRegistry::SetMounted(a_rider, false);
        }
        return started;
    }

    bool IsSeated(const RemoteActor& a_rider, const RemoteActor& a_mount)
    {
        auto riderPtr = a_rider.handle.get();
        auto mountPtr = a_mount.handle.get();
        RE::NiPointer<RE::Actor> currentMount;
        return riderPtr && mountPtr && riderPtr->GetMount(currentMount) && currentMount.get() == mountPtr.get();
    }

    bool Dismount(const RemoteActor& a_rider)
    {
        if (!GameThread::Require("DismountRemoteRider") || !REL::Module::IsAE())
        {
            return false;
        }

        auto riderPtr = a_rider.handle.get();
        if (!riderPtr)
        {
            return false;
        }
        // Mode 0 is the AI dismount procedure's: the exit clip plays, and the
        // interaction ends when it does. Modes 1 and 2 are quick and forced.
        using DismountActor = bool(RE::Actor*, std::int32_t);
        static REL::Relocation<DismountActor> dismount{ REL::ID(38705) };
        constexpr std::int32_t kAnimatedDismount = 0;
        return dismount(riderPtr.get(), kAnimatedDismount);
    }

    void ReleaseRider(RemoteActor& a_rider)
    {
        ActorRegistry::SetMounted(a_rider, false);
        if (a_rider.animation)
        {
            ActorAnimation::InvalidateAnimationDispatch(*a_rider.animation);
        }
    }

    bool IsInTransition(const RemoteActor& a_rider, const RemoteActor* a_mount)
    {
        auto riderPtr = a_rider.handle.get();
        auto mountPtr = a_mount ? a_mount->handle.get() : RE::NiPointer<RE::Actor>{};
        return (riderPtr && (IsGettingOnOrOff(*riderPtr) || IsRiderClipPlaying(*riderPtr)))
            || (mountPtr && IsGettingOnOrOff(*mountPtr));
    }

    bool IsGraphMoving(const RemoteActor& a_mount)
    {
        static const RE::BSFixedString isMovingVariable{ "IsMoving" };
        auto mountPtr = a_mount.handle.get();
        bool moving = false;
        return mountPtr && mountPtr->GetGraphVariableBool(isMovingVariable, moving) && moving;
    }

    std::uint32_t GetSitSleepState(const RemoteActor& a_rider)
    {
        auto riderPtr = a_rider.handle.get();
        const auto* actorState = riderPtr ? riderPtr->AsActorState() : nullptr;
        return actorState ? static_cast<std::uint32_t>(actorState->GetSitSleepState()) : 0;
    }

    void ApplyGait(RE::Actor& a_actor, const RemoteActor& a_remote, Gait& a_gait, float a_speed, float a_deltaTime,
        bool a_sendEvents, const char* a_role)
    {
        PROFILE_FUNCTION();
        static const RE::BSFixedString speedVariable{ "Speed" };
        static const RE::BSFixedString horseSpeedSampledVariable{ "HorseSpeedSampled" };

        const float deltaTime = std::clamp(a_deltaTime, 0.0F, ActorConstants::kMaximumFrameTime);
        const bool moving = a_speed >= kMovingSpeed;
        const bool inGallopRange = moving && a_speed >= (a_gait.sprinting ? kGallopStopSpeed : kGallopStartSpeed);
        // A single fast sample, such as the relocation onto a new mount, must not
        // start a gallop.
        a_gait.gallopPendingElapsed
            = inGallopRange && !a_gait.sprinting ? a_gait.gallopPendingElapsed + deltaTime : 0.0F;
        const bool sprinting = inGallopRange && (a_gait.sprinting || a_gait.gallopPendingElapsed >= kGallopHoldSeconds);

        const bool firstUpdate = a_gait.graphSpeed < 0.0F;
        if (a_speed != a_gait.graphSpeed)
        {
            // Written once per new checkpoint rather than every pass.
            static const RE::BSFixedString directionVariable{ "Direction" };
            ActorRegistry::SetGaitSpeed(a_remote, a_speed);
            (void)a_actor.SetGraphVariableFloat(speedVariable, a_speed);
            // A mount only travels the way it faces.
            (void)a_actor.SetGraphVariableFloat(directionVariable, 0.0F);
            a_gait.graphSpeed = a_speed;
        }
        if (firstUpdate || sprinting != a_gait.sprinting)
        {
            static const RE::BSFixedString isSprintingVariable{ "IsSprinting" };
            (void)a_actor.SetGraphVariableBool(isSprintingVariable, sprinting);
        }
        if (!a_sendEvents)
        {
            a_gait.moving = moving;
            a_gait.sprinting = sprinting;
        }
        else
        {
            // The gallop is a state inside locomotion, which resumes it on its next
            // moveStart, so the gallop is left before locomotion is. The request is
            // recorded even when the graph rejects it; reconciliation retries it.
            if (a_gait.sprinting && !sprinting)
            {
                const bool accepted = SendGaitEvent(a_actor, a_gait, "SprintStop", a_role);
                a_gait.sprinting = false;
                logs::debug("Remote {} gait changed: actor={:08X} sprinting=false accepted={} speed={:.1f}", a_role,
                    a_actor.GetHandle().native_handle(), accepted, a_speed);
            }
            if (moving != a_gait.moving)
            {
                const bool accepted = SendGaitEvent(a_actor, a_gait, moving ? "moveStart" : "moveStop", a_role);
                a_gait.moving = moving;
                logs::debug("Remote {} gait changed: actor={:08X} moving={} accepted={} speed={:.1f}", a_role,
                    a_actor.GetHandle().native_handle(), moving, accepted, a_speed);
            }
            if (a_gait.moving && sprinting && !a_gait.sprinting)
            {
                const bool accepted = SendGaitEvent(a_actor, a_gait, "SprintStart", a_role);
                a_gait.sprinting = true;
                logs::debug("Remote {} gait changed: actor={:08X} sprinting=true accepted={} speed={:.1f}", a_role,
                    a_actor.GetHandle().native_handle(), accepted, a_speed);
            }

            // The events above are edges, and the graph may still disagree, for example
            // with a gallop resumed by moveStart. Its own state decides the correction.
            a_gait.reconcileElapsed += deltaTime;
            if (a_gait.reconcileElapsed >= kReconcileSeconds)
            {
                a_gait.reconcileElapsed = 0.0F;
                static const RE::BSFixedString isMovingVariable{ "IsMoving" };
                static const RE::BSFixedString stateVariable{ "iState" };
                static const RE::BSFixedString sprintStateVariable{ "iState_HorseSprint" };
                bool graphMoving = false;
                std::int32_t graphState = 0;
                std::int32_t sprintState = 0;
                // IsMoving is set while the locomotion state is active, and the gallop
                // state tags iState with iState_HorseSprint.
                if (a_actor.GetGraphVariableBool(isMovingVariable, graphMoving)
                    && a_actor.GetGraphVariableInt(stateVariable, graphState)
                    && a_actor.GetGraphVariableInt(sprintStateVariable, sprintState))
                {
                    const bool graphSprinting = graphState == sprintState;
                    const char* correction = nullptr;
                    if (graphSprinting && !a_gait.sprinting)
                    {
                        correction = "SprintStop";
                    }
                    else if (a_gait.moving && !graphMoving)
                    {
                        correction = "moveStart";
                    }
                    else if (!a_gait.moving && graphMoving)
                    {
                        correction = "moveStop";
                    }
                    else if (graphMoving && a_gait.sprinting && !graphSprinting)
                    {
                        correction = "SprintStart";
                    }
                    if (correction)
                    {
                        const bool accepted = SendGaitEvent(a_actor, a_gait, correction, a_role);
                        const auto corrections = ++a_gait.corrections;
                        if (corrections <= kInitialBlockedEventLogs || corrections % kBlockedEventLogInterval == 0)
                        {
                            logs::debug(
                                "Corrected remote {} graph gait: actor={:08X} event={} accepted={} speed={:.1f} "
                                "graphMoving={} graphSprinting={} horseSpeedSampled={:.1f} count={}",
                                a_role, a_actor.GetHandle().native_handle(), correction, accepted, a_speed, graphMoving,
                                graphSprinting, GetGraphFloat(a_actor, horseSpeedSampledVariable), corrections);
                        }
                    }
                }
            }
        }

        if (!a_gait.moving)
        {
            a_gait.diagnosticElapsed = 0.0F;
            return;
        }
        a_gait.diagnosticElapsed += deltaTime;
        if (a_gait.diagnosticElapsed >= kGaitDiagnosticSeconds)
        {
            a_gait.diagnosticElapsed = 0.0F;
            logs::debug(
                "Remote {} gait sample: actor={:08X} speed={:.1f} graphSpeed={:.1f} horseSpeedSampled={:.1f} "
                "sprinting={}",
                a_role, a_actor.GetHandle().native_handle(), a_speed, GetGraphFloat(a_actor, speedVariable),
                GetGraphFloat(a_actor, horseSpeedSampledVariable), a_gait.sprinting);
        }
    }
}
