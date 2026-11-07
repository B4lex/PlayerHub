#include "actor/ActorFlight.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRegistry.h"
#include "actor/AnimationFirewall.h"
#include "actor/RemoteActor.h"
#include "runtime/FrameTime.h"
#include "runtime/Profiling.h"

#include <cmath>
#include <cstdint>

namespace ActorFlight
{
    namespace
    {
        using ActorConstants::ShouldLogOccurrence;
        using Multiplayer::FlyState;
        using Multiplayer::GetFlyStateName;
        using Multiplayer::GetWingStateName;
        using Multiplayer::WingState;

        constexpr float kReconcileSeconds = 0.25F;
        // A graph that rejects a correction is mid-transition, such as a landing, whose states
        // keep iState_DragonFlying through the touchdown seconds after the source reports none.
        constexpr float kRejectedReconcileSeconds = 2.0F;
        // A takeoff or landing clip plays for seconds before the graph reaches its new state,
        // and a correction sent into one would restart it.
        constexpr float kReconcileHoldSeconds = 1.5F;
        constexpr float kPitchRateSmoothingSeconds = 0.2F;
        constexpr float kMovingSpeed = 20.0F;
        constexpr float kDiagnosticSeconds = 2.0F;
        constexpr float kRadiansToDegrees = 57.2957795130823208768F;
        // How far inside its band the receiver puts a flap's or feather's speed ratio, so the
        // graph's one-update-late TargetSpeedDamped still lands in it.
        constexpr float kWingBandMargin = 0.9F;

        // dragonbehavior.hkx's EEM_Wing_State fires Flap while Speed is below TargetSpeedDamped
        // times TargetSpeedThresholdMin (0.9), Feather while above TargetSpeedThresholdMax
        // (1.1) times it, and Glide between. TargetSpeedDamped is TargetSpeed capped by a turn
        // and pitch limit no lower than MinSpeed (2500). Its modifiers then raise Speed while
        // it flaps and lower it by drag, so a source dragon (TargetSpeed 2000) flaps every few
        // seconds.
        const RE::BSFixedString& GetSpeedVariable()
        {
            static const RE::BSFixedString variable{ "Speed" };
            return variable;
        }

        const RE::BSFixedString& GetTargetSpeedDampedVariable()
        {
            static const RE::BSFixedString variable{ "TargetSpeedDamped" };
            return variable;
        }

        const RE::BSFixedString& GetFlapThresholdVariable()
        {
            static const RE::BSFixedString variable{ "TargetSpeedThresholdMin" };
            return variable;
        }

        const RE::BSFixedString& GetFeatherThresholdVariable()
        {
            static const RE::BSFixedString variable{ "TargetSpeedThresholdMax" };
            return variable;
        }

        enum class GraphFlight
        {
            kUnknown,
            kGround,
            kFlying,
            kHovering
        };

        const char* GetGraphFlightName(GraphFlight a_flight)
        {
            switch (a_flight)
            {
                case GraphFlight::kGround:
                    return "ground";
                case GraphFlight::kFlying:
                    return "flying";
                case GraphFlight::kHovering:
                    return "hovering";
                case GraphFlight::kUnknown:
                    break;
            }
            return "unknown";
        }

        // dragonbehavior.hkx tags its flight states' iState with iState_DragonFlying and its
        // hover states' with iState_DragonHovering.
        GraphFlight ReadGraphFlight(RE::Actor& a_actor)
        {
            static const RE::BSFixedString stateVariable{ "iState" };
            static const RE::BSFixedString flyingVariable{ "iState_DragonFlying" };
            static const RE::BSFixedString hoveringVariable{ "iState_DragonHovering" };
            std::int32_t state = 0;
            std::int32_t flying = 0;
            std::int32_t hovering = 0;
            if (!a_actor.GetGraphVariableInt(stateVariable, state) || !a_actor.GetGraphVariableInt(flyingVariable, flying)
                || !a_actor.GetGraphVariableInt(hoveringVariable, hovering))
            {
                return GraphFlight::kUnknown;
            }
            return state == flying ? GraphFlight::kFlying
                : state == hovering ? GraphFlight::kHovering
                                    : GraphFlight::kGround;
        }

        // Unknown while a takeoff or landing plays.
        GraphFlight GetExpectedGraphFlight(FlyState a_state)
        {
            switch (a_state)
            {
                case FlyState::kNone:
                case FlyState::kPerching:
                    return GraphFlight::kGround;
                case FlyState::kCruising:
                case FlyState::kAction:
                    return GraphFlight::kFlying;
                case FlyState::kHovering:
                    return GraphFlight::kHovering;
                case FlyState::kTakeOff:
                case FlyState::kLanding:
                    break;
            }
            return GraphFlight::kUnknown;
        }

        // The events of Skyrim.esm's dragon flight idles: FlyStartTakeOff, FlyStartCruise,
        // FlyStartHover, HoverStart, HoverStop, and FlyStopDefault. A takeoff carries on into
        // flight by itself, and a landing onto the ground.
        const char* GetTransitionEvent(FlyState a_from, FlyState a_to)
        {
            switch (a_to)
            {
                case FlyState::kTakeOff:
                    return "TakeOff";
                case FlyState::kCruising:
                case FlyState::kAction:
                    if (a_from == FlyState::kHovering)
                    {
                        return "HoverStop";
                    }
                    return a_from == FlyState::kTakeOff || a_from == FlyState::kCruising || a_from == FlyState::kAction
                        ? nullptr
                        : "FlyStartCruise";
                case FlyState::kHovering:
                    return a_from == FlyState::kTakeOff || a_from == FlyState::kCruising
                            || a_from == FlyState::kAction
                        ? "HoverStart"
                        : "FlyStartHover";
                case FlyState::kLanding:
                    return "FlyStopDefault";
                case FlyState::kNone:
                case FlyState::kPerching:
                    // A landing skipped by the playout, as after a hitch, still lands.
                    return a_from == FlyState::kCruising || a_from == FlyState::kHovering
                            || a_from == FlyState::kAction
                        ? "FlyStopDefault"
                        : nullptr;
            }
            return nullptr;
        }

        const char* GetCorrectionEvent(GraphFlight a_expected, GraphFlight a_graph)
        {
            if (a_expected == a_graph || a_expected == GraphFlight::kUnknown || a_graph == GraphFlight::kUnknown)
            {
                return nullptr;
            }
            switch (a_expected)
            {
                case GraphFlight::kFlying:
                    return a_graph == GraphFlight::kHovering ? "HoverStop" : "FlyStartCruise";
                case GraphFlight::kHovering:
                    return a_graph == GraphFlight::kFlying ? "HoverStart" : "FlyStartHover";
                case GraphFlight::kGround:
                    return "FlyStopDefault";
                case GraphFlight::kUnknown:
                    break;
            }
            return nullptr;
        }

        bool SendFlightEvent(RE::Actor& a_actor, Flight& a_flight, const char* a_eventName)
        {
            const RE::BSFixedString eventName{ a_eventName };
            const bool accepted = AnimationFirewall::NotifyNetworkAnimationGraph(a_actor, eventName);
            if (!accepted)
            {
                const auto rejectedEvents = ++a_flight.rejectedEvents;
                if (ShouldLogOccurrence(rejectedEvents))
                {
                    logs::debug("Remote flying mount graph rejected flight event: actor={:08X} event={} count={}",
                        a_actor.GetHandle().native_handle(), a_eventName, rejectedEvents);
                }
            }
            return accepted;
        }

        float GetGraphFloat(RE::Actor& a_actor, const char* a_name)
        {
            float value = 0.0F;
            return a_actor.GetGraphVariableFloat(RE::BSFixedString{ a_name }, value) ? value : -1.0F;
        }

        bool GetGraphBool(RE::Actor& a_actor, const char* a_name)
        {
            bool value = false;
            return a_actor.GetGraphVariableBool(RE::BSFixedString{ a_name }, value) && value;
        }

        bool ReadWingThresholds(RE::Actor& a_actor, Flight& a_flight)
        {
            if (a_flight.hasWingThresholds)
            {
                return true;
            }
            float flap = 0.0F;
            float feather = 0.0F;
            if (!a_actor.GetGraphVariableFloat(GetFlapThresholdVariable(), flap)
                || !a_actor.GetGraphVariableFloat(GetFeatherThresholdVariable(), feather) || !std::isfinite(flap)
                || !std::isfinite(feather) || flap <= 0.0F || feather <= flap)
            {
                return false;
            }
            a_flight.flapThreshold = flap;
            a_flight.featherThreshold = feather;
            a_flight.hasWingThresholds = true;
            return true;
        }

        // The graph picks the wing stroke from Speed over TargetSpeedDamped, so a TargetSpeed
        // that puts the measured speed's ratio inside the source's band replays its stroke.
        // Speed stays the measured speed, which the actor's movement may read.
        float GetWingTargetSpeed(RE::Actor& a_actor, Flight& a_flight, FlyState a_state, WingState a_wing,
            float a_speed)
        {
            if ((a_state != FlyState::kCruising && a_state != FlyState::kAction) || a_wing == WingState::kNone
                || !ReadWingThresholds(a_actor, a_flight))
            {
                return a_speed;
            }
            float ratio = (a_flight.flapThreshold + a_flight.featherThreshold) * 0.5F;
            if (a_wing == WingState::kFlapping)
            {
                ratio = a_flight.flapThreshold * kWingBandMargin;
            }
            else if (a_wing == WingState::kFeathering)
            {
                ratio = a_flight.featherThreshold / kWingBandMargin;
            }
            return a_speed / ratio;
        }

        void UpdateChannels(RE::Actor& a_actor, const RemoteActor& a_remote, Flight& a_flight, FlyState a_state,
            WingState a_wing, float a_speed, float a_deltaTime)
        {
            const auto& movement = a_remote.movement;
            const float targetPitch = movement.targetPitch;
            if (a_flight.hasPreviousTargetPitch && a_deltaTime > 0.0F)
            {
                const float rate = (targetPitch - a_flight.previousTargetPitch) / a_deltaTime;
                const float blend = 1.0F - std::exp(-a_deltaTime / kPitchRateSmoothingSeconds);
                a_flight.pitchRate += (rate - a_flight.pitchRate) * blend;
            }
            a_flight.previousTargetPitch = targetPitch;
            a_flight.hasPreviousTargetPitch = true;

            const float turnRate = std::isfinite(movement.targetHeadingRate) ? movement.targetHeadingRate : 0.0F;
            ActorRegistry::SetFlightChannels(a_remote, ActorRegistry::FlightChannels{
                .targetSpeed = GetWingTargetSpeed(a_actor, a_flight, a_state, a_wing, a_speed),
                // TurnDelta runs against Skyrim's heading: a source dragon turning at -75 degrees
                // per second read +75 (seen in game).
                .turnDelta = -turnRate * kRadiansToDegrees,
                .pitchDelta = a_flight.pitchRate * kRadiansToDegrees });
            // The Speed channel follows the flight speed, as a flying actor's own movement would.
            ActorRegistry::SetGaitSpeed(a_remote, a_speed);
        }

        void LogSample(RE::Actor& a_actor, const Flight& a_flight, WingState a_wing, float a_speed)
        {
            const auto channels = ActorRegistry::GetFlightChannels(&a_actor);
            const auto* controller = a_actor.GetCharController();
            std::int32_t state = -1;
            (void)a_actor.GetGraphVariableInt(RE::BSFixedString{ "iState" }, state);
            logs::debug(
                "Remote flying mount sample: actor={:08X} flyState={} wing={} graph={} iState={} speed={:.1f} "
                "graphSpeed={:.1f} targetSpeed={:.1f} targetSpeedDamped={:.1f} turnDelta={:.3f} pitchDelta={:.3f} "
                "sentTurnDelta={:.3f} sentPitchDelta={:.3f} flapping={} gliding={} feathering={} pitch={:.3f} "
                "angleX={:.3f} moveDirZ={:.3f} flightPitchBlend={:.3f} canPitch={}",
                a_actor.GetHandle().native_handle(), GetFlyStateName(a_flight.requested), GetWingStateName(a_wing),
                GetGraphFlightName(ReadGraphFlight(a_actor)), state, a_speed, GetGraphFloat(a_actor, "Speed"),
                GetGraphFloat(a_actor, "TargetSpeed"), GetGraphFloat(a_actor, "TargetSpeedDamped"),
                GetGraphFloat(a_actor, "TurnDelta"),
                GetGraphFloat(a_actor, "PitchDelta"), channels ? channels->turnDelta : 0.0F,
                channels ? channels->pitchDelta : 0.0F, GetGraphBool(a_actor, "IsFlapping"),
                GetGraphBool(a_actor, "IsGliding"), GetGraphBool(a_actor, "IsFeathering"),
                controller ? controller->pitchAngle : 0.0F, a_actor.GetAngleX(), GetGraphFloat(a_actor, "MoveDirZ"),
                GetGraphFloat(a_actor, "FlightPitchBlend"),
                controller && controller->flags.any(RE::CHARACTER_FLAGS::kCanPitch));
        }

        void LogSampleWhenDue(RE::Actor& a_actor, Flight& a_flight, WingState a_wing, float a_speed, float a_deltaTime)
        {
            if (a_flight.requested == FlyState::kNone)
            {
                a_flight.diagnosticElapsed = 0.0F;
                return;
            }
            a_flight.diagnosticElapsed += a_deltaTime;
            if (a_flight.diagnosticElapsed >= kDiagnosticSeconds)
            {
                a_flight.diagnosticElapsed = 0.0F;
                LogSample(a_actor, a_flight, a_wing, a_speed);
            }
        }

        // A takeoff refuses a landing until it reaches flight, and a landing a takeoff until it
        // touches down, so a short hop's edges wait for the clip before them.
        void RetryPendingEvent(RE::Actor& a_actor, Flight& a_flight)
        {
            if (a_flight.reconcileElapsed < kReconcileSeconds)
            {
                return;
            }
            a_flight.reconcileElapsed = 0.0F;
            const auto expected = GetExpectedGraphFlight(a_flight.requested);
            if (expected != GraphFlight::kUnknown && expected == ReadGraphFlight(a_actor))
            {
                a_flight.pendingEvent = nullptr;
                return;
            }
            if (!SendFlightEvent(a_actor, a_flight, a_flight.pendingEvent))
            {
                return;
            }
            logs::debug("Remote flying mount graph took a retried flight event: actor={:08X} flyState={} event={} "
                        "heldSeconds={:.2f}",
                a_actor.GetHandle().native_handle(), GetFlyStateName(a_flight.requested), a_flight.pendingEvent,
                a_flight.requestedElapsed);
            a_flight.pendingEvent = nullptr;
            // The reconcile hold covers the clip the event started.
            a_flight.requestedElapsed = 0.0F;
        }
    }

    WingState ReadWingState(RE::Actor& a_mount, FlyState a_state)
    {
        if (a_state != FlyState::kCruising && a_state != FlyState::kAction)
        {
            return WingState::kNone;
        }
        float speed = 0.0F;
        float targetSpeed = 0.0F;
        float flap = 0.0F;
        float feather = 0.0F;
        if (!a_mount.GetGraphVariableFloat(GetSpeedVariable(), speed)
            || !a_mount.GetGraphVariableFloat(GetTargetSpeedDampedVariable(), targetSpeed)
            || !a_mount.GetGraphVariableFloat(GetFlapThresholdVariable(), flap)
            || !a_mount.GetGraphVariableFloat(GetFeatherThresholdVariable(), feather) || !std::isfinite(speed)
            || !std::isfinite(targetSpeed) || targetSpeed <= 0.0F)
        {
            return WingState::kNone;
        }
        return speed < targetSpeed * flap ? WingState::kFlapping
            : speed > targetSpeed * feather ? WingState::kFeathering
                                            : WingState::kGliding;
    }

    void Apply(RE::Actor& a_actor, const RemoteActor& a_remote, Flight& a_flight, FlyState a_state, WingState a_wing,
        float a_speed, float a_deltaTime)
    {
        PROFILE_FUNCTION();
        const float deltaTime = FrameTime::Clamp(a_deltaTime);
        UpdateChannels(a_actor, a_remote, a_flight, a_state, a_wing, a_speed, deltaTime);
        a_flight.moving = a_speed >= kMovingSpeed;

        if (!a_flight.started || a_state != a_flight.requested)
        {
            const auto from = a_flight.started ? a_flight.requested : FlyState::kNone;
            const char* eventName = GetTransitionEvent(from, a_state);
            const bool accepted = eventName && SendFlightEvent(a_actor, a_flight, eventName);
            if (eventName || from != a_state)
            {
                logs::debug("Remote flying mount state changed: actor={:08X} from={} to={} event={} accepted={} "
                            "speed={:.1f}",
                    a_actor.GetHandle().native_handle(), GetFlyStateName(from), GetFlyStateName(a_state),
                    eventName ? eventName : "none", accepted, a_speed);
            }
            a_flight.started = true;
            a_flight.requested = a_state;
            a_flight.pendingEvent = eventName && !accepted ? eventName : nullptr;
            a_flight.requestedElapsed = 0.0F;
            a_flight.reconcileElapsed = 0.0F;
            a_flight.reconcileInterval = kReconcileSeconds;
        }
        else
        {
            a_flight.requestedElapsed += deltaTime;
        }

        a_flight.reconcileElapsed += deltaTime;
        if (a_flight.pendingEvent)
        {
            RetryPendingEvent(a_actor, a_flight);
        }
        // An edge may be rejected mid-transition, or a state reached on a branch the edge did
        // not expect; the graph's own state decides the correction.
        else if (a_flight.requestedElapsed >= kReconcileHoldSeconds
            && a_flight.reconcileElapsed >= a_flight.reconcileInterval)
        {
            a_flight.reconcileElapsed = 0.0F;
            const auto expected = GetExpectedGraphFlight(a_flight.requested);
            const auto graph = ReadGraphFlight(a_actor);
            if (const char* correction = GetCorrectionEvent(expected, graph))
            {
                const bool accepted = SendFlightEvent(a_actor, a_flight, correction);
                a_flight.reconcileInterval = accepted ? kReconcileSeconds : kRejectedReconcileSeconds;
                const auto corrections = ++a_flight.corrections;
                if (ShouldLogOccurrence(corrections))
                {
                    logs::debug("Corrected remote flying mount graph: actor={:08X} flyState={} graph={} event={} "
                                "accepted={} count={}",
                        a_actor.GetHandle().native_handle(), GetFlyStateName(a_flight.requested),
                        GetGraphFlightName(graph), correction, accepted, corrections);
                }
            }
        }

        LogSampleWhenDue(a_actor, a_flight, a_wing, a_speed, deltaTime);
    }
}
