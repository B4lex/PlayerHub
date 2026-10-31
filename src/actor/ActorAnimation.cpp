#include "actor/ActorAnimation.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRegistry.h"
#include "actor/AnimationFirewall.h"
#include "actor/GraphStance.h"
#include "actor/RemoteActor.h"
#include "runtime/FrameTime.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace ActorAnimation
{
    namespace
    {
        using Multiplayer::IsMoving;
        using Multiplayer::IsSneaking;
        using Multiplayer::IsSprinting;

        constexpr std::uint32_t kMaximumEventDispatchAttempts = 4;
        // The graph refuses locomotion while it is still leaving an attack, and a
        // power attack's exit was measured holding it past 0.1 seconds.
        constexpr float kLocomotionEventRetrySeconds = 0.75F;
        // Skyrim cross-blends directional locomotion (CyclicFreeze, then
        // CyclicCrossBlend) instead of sweeping through intermediate directions.
        constexpr float kDirectionCrossBlendThreshold = 0.125F;
        constexpr float kDirectionCrossBlendIntervalSeconds = 0.25F;
        // Longer than a held stop's zero-speed tail (the stop hold plus one checkpoint),
        // shorter than running in place against a wall or through a load.
        constexpr float kZeroSpeedHoldSeconds = 0.25F;
        // The idle turn state machines of the vanilla behaviors (MT_Idle_TurnBehavior,
        // 1HM_Idle_Behavior, Bow_Idle_Behavior, MagicReadyIdleTurnBehavior, ...) number
        // their states 0 turn right, 1 standing, and 2 turn left.
        constexpr std::int32_t kStandingTurnState = 1;
        constexpr float kUnsettledTranslationHoldSeconds = 0.15F;
        constexpr float kLandingCompletionFallbackSeconds = 2.0F;
        // SwimIdle rejects moveStart until the graph has taken SwimStart.
        constexpr float kSwimMoveStartFallbackSeconds = 0.5F;
        constexpr float kGraphStanceSampleSeconds = 0.1F;
        // A sneakStop keeps the sneak branch active while it blends out.
        constexpr float kGraphSneakMismatchSeconds = 0.5F;
        // A standing/moving selector switches as the edge is taken, with no blend to
        // wait out; two samples rule out a single update between edge and switch.
        constexpr float kGraphStandingMismatchSeconds = 0.2F;
        constexpr std::uint32_t kMaximumGraphStanceCorrections = 3;
#ifndef NDEBUG
        constexpr float kGraphProbeSettleSeconds = 0.5F;
#endif

        // Counting passes rather than seconds is intended: this only throttles the log.
        constexpr bool ShouldReportEventRetry(std::uint32_t a_attempt) noexcept
        {
            constexpr std::uint32_t kEventRetryReportInterval = 60;
            return a_attempt == 1 || a_attempt % kEventRetryReportInterval == 0;
        }

        static_assert(ShouldReportEventRetry(1));
        static_assert(!ShouldReportEventRetry(2));
        static_assert(ShouldReportEventRetry(60));

        constexpr Multiplayer::MovementState ToggleSneak(Multiplayer::MovementState a_state) noexcept
        {
            using Multiplayer::MovementState;
            switch (a_state)
            {
                case MovementState::kIdle:
                    return MovementState::kSneakingIdle;
                case MovementState::kWalking:
                    return MovementState::kSneakingWalking;
                case MovementState::kRunning:
                    return MovementState::kSneakingRunning;
                case MovementState::kSneakingIdle:
                    return MovementState::kIdle;
                case MovementState::kSneakingWalking:
                    return MovementState::kWalking;
                case MovementState::kSneakingRunning:
                    return MovementState::kRunning;
                default:
                    return a_state;
            }
        }

        static_assert(
            ToggleSneak(Multiplayer::MovementState::kRunning) == Multiplayer::MovementState::kSneakingRunning);
        static_assert(ToggleSneak(Multiplayer::MovementState::kSneakingIdle) == Multiplayer::MovementState::kIdle);
        static_assert(ToggleSneak(Multiplayer::MovementState::kSprinting) == Multiplayer::MovementState::kSprinting);

        constexpr Multiplayer::MovementState ToStanding(Multiplayer::MovementState a_state) noexcept
        {
            return IsSneaking(a_state) ? Multiplayer::MovementState::kSneakingIdle : Multiplayer::MovementState::kIdle;
        }

        static_assert(
            ToStanding(Multiplayer::MovementState::kSneakingRunning) == Multiplayer::MovementState::kSneakingIdle);
        static_assert(ToStanding(Multiplayer::MovementState::kRunning) == Multiplayer::MovementState::kIdle);

        constexpr const char* GetJumpName(bool a_jumping) noexcept
        {
            return a_jumping ? "jumping" : "grounded";
        }

        // The outcome of one locomotion axis's edge in a dispatch.
        enum class Edge : std::uint8_t
        {
            kNone,
            kPending,
            kSent,
            // The graph already held the state.
            kSkipped,
            // Another accepted edge entered the state.
            kImplied,
            kRejected
        };

        constexpr bool IsEdgeDone(Edge a_edge) noexcept
        {
            return a_edge == Edge::kNone || a_edge == Edge::kSent || a_edge == Edge::kSkipped
                || a_edge == Edge::kImplied;
        }

        constexpr const char* GetEdgeName(Edge a_edge) noexcept
        {
            switch (a_edge)
            {
                case Edge::kPending:
                    return "pending";
                case Edge::kSent:
                    return "sent";
                case Edge::kSkipped:
                    return "skipped";
                case Edge::kImplied:
                    return "implied";
                case Edge::kRejected:
                    return "rejected";
                default:
                    return "-";
            }
        }

        struct EdgeResults
        {
            Edge move{ Edge::kNone };
            Edge sneak{ Edge::kNone };
            Edge sprint{ Edge::kNone };
            Edge jump{ Edge::kNone };

            [[nodiscard]] constexpr bool AreDone() const noexcept
            {
                return IsEdgeDone(move) && IsEdgeDone(sneak) && IsEdgeDone(sprint) && IsEdgeDone(jump);
            }
        };

        // The locomotion step one pass dispatches, and the edges it requires.
        struct DispatchPlan
        {
            Multiplayer::AnimationState state{};
            Multiplayer::AnimationState previous{};
            bool hasPrevious{ false };
            std::uint64_t revision{ 0 };
            float graphSpeed{ 0.0F };
            WeaponTransition::Replay weaponReplay{};
            bool dispatchRequired{ false };
            bool reassert{ false };
            bool jumpRequired{ false };
            bool landingInWater{ false };
            bool jumpLandRequired{ false };
            bool moveRequired{ false };
            bool sneakRequired{ false };
            bool sprintRequired{ false };

            [[nodiscard]] constexpr bool RequiresEvents() const noexcept { return dispatchRequired || reassert; }
        };

        enum VariableBit : std::uint32_t
        {
            kSpeedVariable = 1U << 0,
            kSampledSpeedVariable = 1U << 1,
            kDampedSpeedVariable = 1U << 2,
            kDirectionVariable = 1U << 3,
            kTurnVariable = 1U << 4,
            kSprintVariable = 1U << 5,
            kSyncSprintVariable = 1U << 6,
            kSyncIdleVariable = 1U << 7,
            kJumpVariable = 1U << 8
        };

        // A graph without iSyncSprintState or iSyncIdleLocomotion still takes the state.
        constexpr std::uint32_t kRequiredVariables = kSpeedVariable | kSampledSpeedVariable | kDampedSpeedVariable
            | kDirectionVariable | kTurnVariable | kSprintVariable | kJumpVariable;

        float GetGraphSpeedForState(
            RE::Actor& a_actor, float a_sampleSpeed, float a_heldSpeed, Multiplayer::MovementState a_state)
        {
            if (!IsMoving(a_state))
            {
                return 0.0F;
            }

            if (std::isfinite(a_sampleSpeed) && a_sampleSpeed > 0.0F)
            {
                return a_sampleSpeed;
            }
            if (a_heldSpeed > 0.0F)
            {
                return a_heldSpeed;
            }

            const bool walking = a_state == Multiplayer::MovementState::kWalking
                || a_state == Multiplayer::MovementState::kSneakingWalking;
            const float graphSpeed = walking ? a_actor.GetWalkSpeed() : a_actor.GetRunSpeed();
            return std::isfinite(graphSpeed) && graphSpeed > 0.0F ? graphSpeed : 1.0F;
        }

        // The graph probe dumps every active node and variable, so release builds
        // compile it out.
#ifndef NDEBUG
        void LogGraphProbe(RE::Actor& a_actor, RemoteActor& a_remote, const char* a_reason)
        {
            auto& state = a_remote.animation;
            logs::debug("Animation graph probe: actor={:08X} reason={} latched={}/{} weaponDrawn={} iState={} {}",
                a_remote.handle.native_handle(), a_reason,
                Multiplayer::GetMovementStateName(state.dispatch.latched.movementState),
                GetJumpName(state.dispatch.latched.jumping), state.dispatch.latched.weaponDrawn,
                GraphStance::ReadStateTag(a_actor), GraphProbe::Describe(a_actor, state.graphProbeSnapshot));
        }

        void ArmGraphProbe(State& a_state, const char* a_reason)
        {
            a_state.graphProbeReason = a_reason;
            a_state.graphProbeElapsed = 0.0F;
        }

        void AdvanceGraphProbe(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime)
        {
            auto& state = a_remote.animation;
            if (state.dispatch.hasLatched && state.dispatch.latched != state.graphProbeState)
            {
                state.graphProbeState = state.dispatch.latched;
                ArmGraphProbe(state, "state");
            }
            if (!state.graphProbeReason)
            {
                return;
            }
            if (!IsAnimationStateSettled(a_remote) || state.attack.pending || state.dispatch.reassertReason
                || HandPresentation::IsPlaying(a_remote) || WeaponTransition::IsUnsettled(a_remote)
                || IdlePresentation::IsPlaying(a_remote) || state.recovery.inProgress)
            {
                state.graphProbeElapsed = 0.0F;
                return;
            }
            state.graphProbeElapsed += FrameTime::Clamp(a_deltaTime);
            if (state.graphProbeElapsed < kGraphProbeSettleSeconds)
            {
                return;
            }
            LogGraphProbe(a_actor, a_remote, state.graphProbeReason);
            state.graphProbeReason = nullptr;
        }
#else
        void LogGraphProbe(RE::Actor&, RemoteActor&, const char*) {}
        void ArmGraphProbe(State&, const char*) {}
        void AdvanceGraphProbe(RE::Actor&, RemoteActor&, float) {}
#endif

        void ResetGraphDispatch(RE::Actor& a_actor, RemoteActor& a_remote)
        {
            AnimationFirewall::Disable(a_remote);
            ActorRegistry::SetLandingCompletionObservation(a_actor, false);
            InvalidateAnimationDispatch(a_remote.animation);
        }

        void ResetUnavailableGraph(RE::Actor& a_actor, RemoteActor& a_remote, const char* a_reason)
        {
            if (a_remote.animation.dispatch.hasLatched || a_remote.animation.dispatch.graph)
            {
                logs::debug("Animation sync reset graph state: actor={:08X} reason={}", a_remote.handle.native_handle(),
                    a_reason);
            }
            ResetGraphDispatch(a_actor, a_remote);
        }

        // Returns whether the landing output sink watches the graph, or none without a
        // graph to dispatch to.
        std::optional<bool> ObserveGraph(RE::Actor& a_actor, RemoteActor& a_remote)
        {
            if (!a_actor.Is3DLoaded())
            {
                ResetUnavailableGraph(a_actor, a_remote, "actor-or-3d-unavailable");
                return std::nullopt;
            }
            RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
            if (!a_actor.GetAnimationGraphManager(graphManager) || !graphManager)
            {
                ResetUnavailableGraph(a_actor, a_remote, "graph-unavailable");
                return std::nullopt;
            }
            auto& dispatch = a_remote.animation.dispatch;
            if (dispatch.graph != graphManager.get())
            {
                logs::debug("Animation sync detected {} graph: actor={:08X}; next state will be fully asserted",
                    dispatch.graph ? "replacement" : "initial", a_remote.handle.native_handle());
                ResetGraphDispatch(a_actor, a_remote);
                dispatch.graph = graphManager.get();
            }
            if (dispatch.landingSinkGraph == graphManager.get())
            {
                return true;
            }
            if (!AnimationFirewall::RegisterGraphOutputEventSink(*graphManager))
            {
                return false;
            }
            dispatch.landingSinkGraph = graphManager.get();
            logs::debug("Registered actor animation output-event sink: actor={:08X}", a_remote.handle.native_handle());
            return true;
        }

        void AdvanceLanding(RE::Actor& a_actor, RemoteActor& a_remote, bool a_swimming, float a_deltaTime)
        {
            static const RE::BSFixedString jumpLandEndEvent{ "JumpLandEnd" };
            auto& state = a_remote.animation;
            auto& landing = state.landing;
            const auto actorHandle = a_remote.handle.native_handle();
            if (landing.completionPending && state.targetAnimationState.jumping)
            {
                // A consecutive takeoff leaves the landing branch directly, as the
                // local player's graph does. Waiting for JumpLandEnd would hold the
                // takeoff until the remote jump is already over. The jump dispatch
                // closes the landing observation window; grounded locomotion is
                // reasserted after that jump's own landing.
                landing = {};
                logs::debug("Animation sync takeoff preempted pending landing completion: actor={:08X}", actorHandle);
            }
            if (landing.completionPending && a_swimming)
            {
                // SwimStart replaced the landing branch, so its JumpLandEnd marker never
                // arrives. JumpLandEnd is a root wildcard transition: forcing it would pull
                // the graph from Swim_State into land locomotion while Skyrim keeps the
                // actor swimming. Swimming owns locomotion until swimStop.
                landing = {};
                ActorRegistry::SetLandingCompletionObservation(a_actor, false);
                logs::debug("Animation sync landing branch replaced by swimming: actor={:08X}", actorHandle);
            }
            // Without a pending landing this drains a duplicate annotation; the next
            // takeoff also clears the observation window.
            const auto completedLandings = ActorRegistry::ConsumeJumpLandEnds(a_remote);
            if (!landing.completionPending)
            {
                return;
            }

            landing.waitElapsed += FrameTime::Clamp(a_deltaTime);
            bool forcedEventAccepted = false;
            if (completedLandings == 0)
            {
                if (landing.waitElapsed < kLandingCompletionFallbackSeconds)
                {
                    return;
                }
                // A custom behavior graph may omit its normal annotation. Bound the
                // wait and drive the same lifecycle edge explicitly so the remote actor
                // cannot remain in a landing branch indefinitely.
                forcedEventAccepted = SendNetworkAnimationEvent(a_remote, jumpLandEndEvent);
            }

            const float waitedSeconds = landing.waitElapsed;
            landing = {};
            ActorRegistry::SetLandingCompletionObservation(a_actor, false);
            // JumpLandEnd selects the graph's exit branch, which can exit into idle. Keep
            // the dispatch history: forgetting it reads a drawn weapon as a new draw.
            auto& dispatch = state.dispatch;
            dispatch.reassertReason = "landing";
            dispatch.variableAttempts = 0;
            dispatch.eventAttempts = 0;
            dispatch.eventRetryElapsed = 0.0F;
            logs::debug(
                "Animation sync completing landing branch and reasserting grounded locomotion: actor={:08X} "
                "observedMarkers={} waitedSeconds={:.3f} forced={} forcedEventAccepted={}",
                actorHandle, completedLandings, waitedSeconds, completedLandings == 0, forcedEventAccepted);
        }

        void AdvanceSwim(RE::Actor& a_actor, RemoteActor& a_remote, bool a_swimming, float a_deltaTime)
        {
            static const RE::BSFixedString moveStartEvent{ "moveStart" };
            auto& state = a_remote.animation;
            auto& swim = state.swim;
            if (ActorRegistry::ConsumeSwimStarts(a_remote) != 0)
            {
                swim = Swim{ .moveStartPending = true };
            }
            if (ActorRegistry::ConsumeSwimStops(a_remote) != 0)
            {
                state.dispatch.reassertReason = "swim-stop";
                swim = {};
            }
            if (!swim.moveStartPending)
            {
                return;
            }
            const bool movingDispatched = state.dispatch.hasLatched && IsMoving(state.dispatch.latched.movementState)
                && IsMoving(state.targetAnimationState.movementState);
            if (!movingDispatched || !a_swimming)
            {
                swim = {};
                return;
            }

            swim.elapsed += FrameTime::Clamp(a_deltaTime);
            // SwimStart is evaluated on the graph's next update. Counting passes
            // rather than time is intended: the swim branch must have been entered.
            if (++swim.waitPasses < 2)
            {
                return;
            }

            ++swim.attempts;
            const bool alreadyMoving
                = GraphStance::ReadStanceSelectors(a_actor).moving == GraphStance::SelectorState::kOn;
            const bool accepted = alreadyMoving || SendNetworkAnimationEvent(a_remote, moveStartEvent);
            if (!accepted && swim.elapsed < kSwimMoveStartFallbackSeconds)
            {
                return;
            }
            logs::debug(
                "Animation sync selected moving swim state: actor={:08X} state={} accepted={} alreadyMoving={} "
                "attempts={} elapsed={:.3f}",
                a_remote.handle.native_handle(),
                Multiplayer::GetMovementStateName(state.targetAnimationState.movementState), accepted, alreadyMoving,
                swim.attempts, swim.elapsed);
            swim = {};
        }

        // Returns none while the landing sink a landing needs is not registered yet.
        std::optional<DispatchPlan> PlanDispatch(RE::Actor& a_actor, RemoteActor& a_remote, bool a_swimming,
            bool a_weaponTransitionCompleted, bool a_landingSinkRegistered, float a_deltaTime)
        {
            auto& state = a_remote.animation;
            auto& dispatch = state.dispatch;
            const auto& target = state.targetAnimationState;
            DispatchPlan plan{ .revision = state.requestedRevision,
                .weaponReplay = WeaponTransition::PlanReplay(
                    state.weapon, target, dispatch.latched, a_weaponTransitionCompleted) };
            plan.state = plan.weaponReplay.state;
            // A released graph may have left locomotion. Edges it already holds are
            // skipped, so this asserts only the missing ones. An idle holds it, since
            // the idle states take no locomotion edge.
            plan.reassert = dispatch.reassertReason && !state.attack.pending && !a_swimming
                && !IdlePresentation::IsPlaying(a_remote);

            const bool isMoving = IsMoving(plan.state.movementState);
            const bool isSneaking = IsSneaking(plan.state.movementState);
            const bool isSprinting = IsSprinting(plan.state.movementState);
            const auto sampleSpeed = a_remote.movement.animationSpeed;
            const bool zeroSpeedSample = isMoving && !(std::isfinite(sampleSpeed) && sampleSpeed > 0.0F);
            state.zeroSpeedSampleElapsed
                = zeroSpeedSample ? state.zeroSpeedSampleElapsed + FrameTime::Clamp(a_deltaTime) : 0.0F;
            const float heldGraphSpeed
                = state.zeroSpeedSampleElapsed < kZeroSpeedHoldSeconds ? state.publishedGraphSpeed : 0.0F;
            plan.graphSpeed = GetGraphSpeedForState(a_actor, sampleSpeed, heldGraphSpeed, plan.state.movementState);

            plan.hasPrevious = dispatch.hasLatched && !plan.weaponReplay.replayingBase;
            plan.previous = dispatch.latched;
            plan.dispatchRequired = plan.weaponReplay.active || !dispatch.hasLatched || dispatch.latched != plan.state;
            const auto& previous = plan.previous;
            plan.jumpRequired = plan.dispatchRequired
                && (plan.hasPrevious ? previous.jumping != plan.state.jumping : plan.state.jumping);
            // A landing in water sends no JumpLand. The root graph takes it from any state,
            // including Swim_State, and the landing branch then exits into land locomotion.
            plan.landingInWater = plan.jumpRequired && !plan.state.jumping && a_swimming;
            plan.jumpLandRequired = plan.jumpRequired && !plan.state.jumping && !plan.landingInWater;
            if (plan.jumpLandRequired && !a_landingSinkRegistered)
            {
                // Graph construction can expose the manager one pass before its first
                // animation graph becomes available.
                return std::nullopt;
            }

            const bool replaying = plan.weaponReplay.replayingBase || plan.reassert;
            const auto axisRequired = [&](bool a_previous, bool a_current) {
                return !plan.jumpLandRequired
                    && ((plan.dispatchRequired && (plan.hasPrevious ? a_previous != a_current : a_current))
                        || (replaying && a_current));
            };
            plan.moveRequired = axisRequired(IsMoving(previous.movementState), isMoving);
            plan.sneakRequired = axisRequired(IsSneaking(previous.movementState), isSneaking);
            plan.sprintRequired = axisRequired(IsSprinting(previous.movementState), isSprinting);

            if (plan.dispatchRequired && (!dispatch.hasAttempted || dispatch.attempted != plan.state))
            {
                dispatch.attempted = plan.state;
                dispatch.hasAttempted = true;
                dispatch.variableAttempts = 0;
                dispatch.eventAttempts = 0;
                dispatch.eventRetryElapsed = 0.0F;
            }
            return plan;
        }

        // The selector states before any event of the pass, read only when a stance
        // edge is required.
        struct StanceReading
        {
            GraphStance::StanceSelectors selectors{};
            bool read{ false };
        };

        StanceReading ReadStanceForEdges(RE::Actor& a_actor, RemoteActor& a_remote, const DispatchPlan& a_plan)
        {
            using GraphStance::SelectorState;
            if (!a_plan.moveRequired && !a_plan.sneakRequired && !a_plan.sprintRequired)
            {
                return {};
            }
            // Skyrim rejects an edge into the state a selector already holds, which would
            // hold the dispatch for the whole retry window.
            const auto selectors = GraphStance::ReadStanceSelectors(a_actor);
            if ((selectors.moving == SelectorState::kMixed || selectors.sneak == SelectorState::kMixed
                    || selectors.sprinting == SelectorState::kMixed)
                && ShouldReportEventRetry(a_remote.animation.dispatch.eventAttempts + 1))
            {
                logs::debug("Animation sync stance selectors disagree: actor={:08X} desired={} selectors=[{}]",
                    a_remote.handle.native_handle(), Multiplayer::GetMovementStateName(a_plan.state.movementState),
                    GraphStance::DescribeStanceSelectors(a_actor));
            }
            return { .selectors = selectors, .read = true };
        }

        Edge SendEdge(RemoteActor& a_remote, const RE::BSFixedString& a_event, bool a_alreadyThere)
        {
            if (a_alreadyThere)
            {
                return Edge::kSkipped;
            }
            return SendNetworkAnimationEvent(a_remote, a_event) ? Edge::kSent : Edge::kRejected;
        }

        void LogDispatch(RE::Actor& a_actor, const RemoteActor& a_remote, const DispatchPlan& a_plan,
            const StanceReading& a_stance, const EdgeResults& a_edges, const char* a_result)
        {
            const auto& dispatch = a_remote.animation.dispatch;
            logs::debug(
                "Animation sync graph dispatch: actor={:08X} revision={} previous={}/{} desired={}/{} result={} "
                "move={} sneak={} sprint={} jump={} graphSpeed={:.1f} graphMoving={} graphSneak={} "
                "variableAttempt={} eventAttempt={}/{}",
                a_remote.handle.native_handle(), static_cast<unsigned long long>(a_plan.revision),
                a_plan.hasPrevious ? Multiplayer::GetMovementStateName(a_plan.previous.movementState)
                                   : "uninitialized",
                GetJumpName(a_plan.hasPrevious && a_plan.previous.jumping),
                Multiplayer::GetMovementStateName(a_plan.state.movementState), GetJumpName(a_plan.state.jumping),
                a_result, GetEdgeName(a_edges.move), GetEdgeName(a_edges.sneak), GetEdgeName(a_edges.sprint),
                GetEdgeName(a_edges.jump), a_plan.graphSpeed,
                a_stance.read ? GraphStance::GetMovingName(a_stance.selectors.moving) : "unread",
                a_stance.read ? GraphStance::GetSneakName(GraphStance::ReadSneak(a_actor, a_stance.selectors.sneak))
                              : "unread",
                dispatch.variableAttempts + 1, dispatch.eventAttempts + 1, kMaximumEventDispatchAttempts);
        }

        // Stop edges are evaluated against the old graph variables, so they go out
        // before the variable write: clearing Speed or IsSprinting first makes the
        // sprint and idle exits reject and leaves the graph in its previous branch.
        // Returns false while a rejected stop edge is retried.
        bool SendStopEdges(RE::Actor& a_actor, RemoteActor& a_remote, const DispatchPlan& a_plan,
            const StanceReading& a_stance, EdgeResults& a_edges, float a_deltaTime)
        {
            static const RE::BSFixedString moveStopEvent{ "moveStop" };
            static const RE::BSFixedString sneakStopEvent{ "sneakStop" };
            static const RE::BSFixedString sprintStopEvent{ "SprintStop" };
            using GraphStance::SelectorState;

            const auto& selectors = a_stance.selectors;
            if (a_plan.moveRequired && !IsMoving(a_plan.state.movementState))
            {
                a_edges.move = SendEdge(a_remote, moveStopEvent, selectors.moving == SelectorState::kOff);
            }
            // Only the selector states count: the sneak variables also read as not sneaking
            // in branches without a sneak transition, such as weapon equip.
            if (a_plan.sneakRequired && !IsSneaking(a_plan.state.movementState))
            {
                a_edges.sneak = SendEdge(a_remote, sneakStopEvent, selectors.sneak == SelectorState::kOff);
            }
            if (a_plan.sprintRequired && !IsSprinting(a_plan.state.movementState))
            {
                a_edges.sprint = SendEdge(a_remote, sprintStopEvent, selectors.sprinting == SelectorState::kOff);
            }

            auto& dispatch = a_remote.animation.dispatch;
            const auto attempt = dispatch.eventAttempts + 1;
            if (a_edges.AreDone()
                || (attempt >= kMaximumEventDispatchAttempts
                    && dispatch.eventRetryElapsed >= kLocomotionEventRetrySeconds))
            {
                return true;
            }
            if (ShouldReportEventRetry(attempt))
            {
                LogDispatch(a_actor, a_remote, a_plan, a_stance, a_edges, "retrying-stop-events");
            }
            ++dispatch.eventAttempts;
            dispatch.eventRetryElapsed += FrameTime::Clamp(a_deltaTime);
            return false;
        }

        void AlignLanding(RE::Actor& a_actor, RemoteActor& a_remote)
        {
            const auto positionBeforeLanding = a_actor.GetPosition();
            // Clear the interpolation backlog before changing the persistent graph state, or
            // the landing branch can begin while the actor is aloft. The placement target
            // still includes the pending landing position.
            const auto landingPosition = ActorMovement::GetPlacementTarget(a_remote);
            a_remote.movement.landingPosition.reset();
            a_actor.SetPosition(landingPosition, true);
            if (a_remote.animation.dispatch.eventAttempts == 0)
            {
                logs::debug(
                    "Animation sync aligned landing transform: actor={:08X} current=({:.3f},{:.3f},{:.3f}) "
                    "target=({:.3f},{:.3f},{:.3f}) distance={:.1f} zError={:.3f}",
                    a_remote.handle.native_handle(), positionBeforeLanding.x, positionBeforeLanding.y,
                    positionBeforeLanding.z, landingPosition.x, landingPosition.y, landingPosition.z,
                    positionBeforeLanding.GetDistance(landingPosition), landingPosition.z - positionBeforeLanding.z);
            }
        }

        // Returns the variables the graph could not take, as VariableBit flags.
        std::uint32_t WriteVariables(RE::Actor& a_actor, RemoteActor& a_remote, const DispatchPlan& a_plan)
        {
            PROFILE_SCOPE("ApplyAnimationUpdate graph variables");
            static const RE::BSFixedString inJumpStateVariable{ "bInJumpState" };
            static const RE::BSFixedString speedDampedVariable{ "SpeedDamped" };
            auto* strings = RE::FixedStrings::GetSingleton();
            if (!strings)
            {
                return ~0U;
            }

            const bool isMoving = IsMoving(a_plan.state.movementState);
            const bool isSprinting = IsSprinting(a_plan.state.movementState);
            const float graphSpeed = a_plan.graphSpeed;
            std::uint32_t rejected = 0;
            const auto write = [&rejected](bool a_accepted, VariableBit a_bit) {
                rejected |= a_accepted ? 0U : a_bit;
            };
            const bool speedAccepted = a_actor.SetGraphVariableFloat(strings->speed, graphSpeed);
            write(speedAccepted, kSpeedVariable);
            write(a_actor.SetGraphVariableFloat(strings->speedSampled, graphSpeed), kSampledSpeedVariable);
            write(a_actor.SetGraphVariableFloat(speedDampedVariable, graphSpeed), kDampedSpeedVariable);
            // The source's graph keeps its last Direction while standing, and the
            // directional blend stays active then.
            write(a_actor.SetGraphVariableFloat(strings->direction, a_remote.movement.animationDirection),
                kDirectionVariable);
            write(a_actor.SetGraphVariableFloat(strings->turnDelta, 0.0F), kTurnVariable);
            write(a_actor.SetGraphVariableBool(strings->isSprinting, isSprinting), kSprintVariable);
            // The vanilla locomotion, sneak, jump, and landing state machines pick
            // their sprint or non-sprint state from iSyncSprintState when entered,
            // and only some of them write it back. A sprinting takeoff leaves it at
            // sprint, so the moveStart after a running landing would re-enter
            // Sprint_State without a SprintStop.
            write(a_actor.SetGraphVariableInt(strings->iSyncSprintState, isSprinting ? 1 : 0), kSyncSprintVariable);
            // Every vanilla standing/moving machine binds its start state to
            // iSyncIdleLocomotion (0 standing, 1 moving) and no graph node writes it,
            // so on a remote actor it stayed 0 and each branch entered while moving,
            // such as a landing exit, swim, sneak magic, an equip, or a block, bow, or
            // cast lower body, started standing.
            write(a_actor.SetGraphVariableInt(strings->iSyncIdleLocomotion, isMoving ? 1 : 0), kSyncIdleVariable);
            write(a_actor.SetGraphVariableBool(inJumpStateVariable, a_plan.state.jumping), kJumpVariable);

            auto& state = a_remote.animation;
            if (speedAccepted && graphSpeed != state.publishedGraphSpeed)
            {
                ActorRegistry::SetAuthoritativeSpeed(a_remote, graphSpeed);
                state.publishedGraphSpeed = graphSpeed;
            }
            if (rejected != state.dispatch.rejectedVariables)
            {
                const auto accepted = [rejected](VariableBit a_bit) { return (rejected & a_bit) == 0; };
                logs::debug(
                    "Animation sync graph variable refresh changed: actor={:08X} revision={} state={} result={} "
                    "speed={} sampledSpeed={} dampedSpeed={} direction={} turn={} sprint={} syncSprint={} "
                    "syncIdle={} jump={}",
                    a_remote.handle.native_handle(), static_cast<unsigned long long>(a_plan.revision),
                    Multiplayer::GetMovementStateName(a_plan.state.movementState),
                    (rejected & kRequiredVariables) == 0 ? "accepted" : "rejected", accepted(kSpeedVariable),
                    accepted(kSampledSpeedVariable), accepted(kDampedSpeedVariable), accepted(kDirectionVariable),
                    accepted(kTurnVariable), accepted(kSprintVariable), accepted(kSyncSprintVariable),
                    accepted(kSyncIdleVariable), accepted(kJumpVariable));
                state.dispatch.rejectedVariables = rejected;
            }
            return rejected;
        }

        // Takeoff, landing, and locomotion start edges are evaluated against the new
        // graph variables, so they go out after the variable write.
        void SendStartEdges(RE::Actor& a_actor, RemoteActor& a_remote, const DispatchPlan& a_plan,
            const StanceReading& a_stance, bool a_syncSprintAccepted, EdgeResults& a_edges)
        {
            static const RE::BSFixedString moveStartEvent{ "moveStart" };
            static const RE::BSFixedString sneakStartEvent{ "sneakStart" };
            static const RE::BSFixedString sprintStartEvent{ "SprintStart" };
            static const RE::BSFixedString jumpStandingStartEvent{ "JumpStandingStart" };
            static const RE::BSFixedString jumpDirectionalStartEvent{ "JumpDirectionalStart" };
            static const RE::BSFixedString jumpLandEvent{ "JumpLand" };
            using GraphStance::SelectorState;

            const auto& state = a_plan.state;
            const auto& selectors = a_stance.selectors;
            const bool isMoving = IsMoving(state.movementState);
            if (a_plan.moveRequired && isMoving)
            {
                a_edges.move = SendEdge(a_remote, moveStartEvent, selectors.moving == SelectorState::kOn);
            }
            if (a_plan.sneakRequired && IsSneaking(state.movementState))
            {
                // Only positive evidence counts: some sneak branches report neither
                // variable.
                const bool alreadyEntered
                    = GraphStance::ReadSneak(a_actor, selectors.sneak).sneak == GraphStance::Sneak::kSneaking;
                a_edges.sneak = SendEdge(a_remote, sneakStartEvent, alreadyEntered);
            }
            if (a_plan.sprintRequired && IsSprinting(state.movementState))
            {
                a_edges.sprint = SendEdge(a_remote, sprintStartEvent, selectors.sprinting == SelectorState::kOn);
                // A landing exit or an accepted moveStart enters Sprint_State through
                // iSyncSprintState, which leaves SprintStart without an edge.
                if (a_edges.sprint == Edge::kRejected && a_edges.move == Edge::kSent && a_syncSprintAccepted)
                {
                    a_edges.sprint = Edge::kImplied;
                }
            }
            if (a_plan.landingInWater)
            {
                a_edges.jump = Edge::kSkipped;
            }
            else if (a_plan.jumpRequired)
            {
                if (a_plan.jumpLandRequired)
                {
                    ActorRegistry::ArmJumpLandEndObservation(a_actor);
                }
                const auto& jumpEvent
                    = state.jumping ? (isMoving ? jumpDirectionalStartEvent : jumpStandingStartEvent) : jumpLandEvent;
                a_edges.jump = SendEdge(a_remote, jumpEvent, false);
            }
        }

        void LatchDispatch(
            RE::Actor& a_actor, RemoteActor& a_remote, const DispatchPlan& a_plan, bool a_eventAttemptsExhausted)
        {
            auto& state = a_remote.animation;
            auto& dispatch = state.dispatch;
            const auto actorHandle = a_remote.handle.native_handle();
            dispatch.latched = a_plan.state;
            dispatch.hasLatched = true;
            if (WeaponTransition::OnReplayLatched(a_actor, a_remote, a_plan.weaponReplay, a_eventAttemptsExhausted))
            {
                ArmGraphProbe(state, "weapon-replay");
            }
            if (a_plan.reassert)
            {
                logs::debug("Animation sync reasserted locomotion: actor={:08X} reason={} state={}", actorHandle,
                    dispatch.reassertReason, Multiplayer::GetMovementStateName(a_plan.state.movementState));
                dispatch.reassertReason = nullptr;
                ArmGraphProbe(state, "reassert");
            }
            dispatch.variableAttempts = 0;
            dispatch.eventAttempts = 0;
            dispatch.eventRetryElapsed = 0.0F;
            if (a_plan.jumpLandRequired)
            {
                state.landing = Landing{ .completionPending = true };
                logs::debug("Animation sync waiting for landing completion marker: actor={:08X} revision={}",
                    actorHandle, static_cast<unsigned long long>(a_plan.revision));
            }
            else if (a_plan.landingInWater)
            {
                logs::debug("Animation sync skipped landing branch while swimming: actor={:08X} revision={}",
                    actorHandle, static_cast<unsigned long long>(a_plan.revision));
            }
            state.processedRevision = a_plan.revision;
        }

        void DispatchEdges(RE::Actor& a_actor, RemoteActor& a_remote, const DispatchPlan& a_plan,
            const StanceReading& a_stance, std::uint32_t a_rejectedVariables, EdgeResults& a_edges,
            float a_deltaTime)
        {
            auto& dispatch = a_remote.animation.dispatch;
            const bool variablesAccepted = (a_rejectedVariables & kRequiredVariables) == 0;
            if (variablesAccepted)
            {
                SendStartEdges(
                    a_actor, a_remote, a_plan, a_stance, (a_rejectedVariables & kSyncSprintVariable) == 0, a_edges);
            }
            else
            {
                // A required start edge waits for a writable graph.
                const auto markPending = [](Edge& a_edge, bool a_required) {
                    a_edge = a_required && a_edge == Edge::kNone ? Edge::kPending : a_edge;
                };
                markPending(a_edges.move, a_plan.moveRequired);
                markPending(a_edges.sneak, a_plan.sneakRequired);
                markPending(a_edges.sprint, a_plan.sprintRequired);
                markPending(a_edges.jump, a_plan.jumpRequired && !a_plan.landingInWater);
            }

            const bool eventsAccepted = a_edges.AreDone();
            const auto variableAttempt = dispatch.variableAttempts + 1;
            const auto eventAttempt = dispatch.eventAttempts + 1;
            const bool eventAttemptsExhausted = !eventsAccepted && eventAttempt >= kMaximumEventDispatchAttempts
                && dispatch.eventRetryElapsed >= kLocomotionEventRetrySeconds;
            const bool processed = variablesAccepted && (eventsAccepted || eventAttemptsExhausted);
            const bool retryingEvents = variablesAccepted && !processed;
            const bool report = retryingEvents ? ShouldReportEventRetry(eventAttempt)
                                               : variablesAccepted || ShouldReportEventRetry(variableAttempt);
            if (report)
            {
                LogDispatch(a_actor, a_remote, a_plan, a_stance, a_edges,
                    !variablesAccepted           ? "variables-rejected"
                        : eventsAccepted         ? "processed"
                        : eventAttemptsExhausted ? "processed-event-timeout"
                                                 : "retrying-events");
            }

            if (processed)
            {
                LatchDispatch(a_actor, a_remote, a_plan, eventAttemptsExhausted);
            }
            else if (!variablesAccepted)
            {
                ++dispatch.variableAttempts;
            }
            else
            {
                ++dispatch.eventAttempts;
                dispatch.eventRetryElapsed += FrameTime::Clamp(a_deltaTime);
            }
        }

        void AdvanceDirectionBlend(RemoteActor& a_remote, const Multiplayer::AnimationState& a_state,
            bool a_variablesAccepted, float a_deltaTime)
        {
            static const RE::BSFixedString cyclicFreezeEvent{ "CyclicFreeze" };
            static const RE::BSFixedString cyclicCrossBlendEvent{ "CyclicCrossBlend" };
            auto& blend = a_remote.animation.direction;
            const float sampleDirection = a_remote.movement.animationDirection;
            blend.cooldown = (std::max)(blend.cooldown - FrameTime::Clamp(a_deltaTime), 0.0F);
            if (blend.crossBlendPending)
            {
                // One graph update after CyclicFreeze, matching Skyrim's own spacing.
                blend.crossBlendPending = false;
                (void)SendNetworkAnimationEvent(a_remote, cyclicCrossBlendEvent);
                return;
            }
            if (a_state.jumping || !a_variablesAccepted)
            {
                blend.hasDirection = false;
                return;
            }
            // Standing keeps the cross-blended direction: the directional
            // BSCyclicBlendTransitionGenerator stays active while the actor stands (observed in
            // game), so a start in another direction is a change like one while moving.
            if (!IsMoving(a_state.movementState))
            {
                return;
            }
            if (!blend.hasDirection)
            {
                blend.direction = sampleDirection;
                blend.hasDirection = true;
                return;
            }
            const float directionChange = std::abs(sampleDirection - blend.direction);
            const float cyclicDirectionChange = (std::min)(directionChange, 1.0F - directionChange);
            if (cyclicDirectionChange >= kDirectionCrossBlendThreshold && blend.cooldown <= 0.0F)
            {
                (void)SendNetworkAnimationEvent(a_remote, cyclicFreezeEvent);
                blend.direction = sampleDirection;
                blend.crossBlendPending = true;
                blend.cooldown = kDirectionCrossBlendIntervalSeconds;
            }
        }

        // Skyrim's event result is not an acknowledgement, so a sneak or move edge
        // that ran out of retries is latched as taken whether or not the graph took
        // it, and a branch entered standing leaves the graph standing under a moving
        // latch. Later dispatches only send an edge when the latched state changes.
        void ReconcileGraphStance(RE::Actor& a_actor, RemoteActor& a_remote, bool a_swimming, float a_deltaTime)
        {
            auto& state = a_remote.animation;
            auto& stance = state.stance;
            auto& dispatch = state.dispatch;
            const auto& target = state.targetAnimationState;
            const bool reconcilable = dispatch.hasLatched && dispatch.latched == target
                && IsAnimationStateSettled(a_remote) && !target.jumping && !IsSprinting(target.movementState)
                && !dispatch.reassertReason && !state.attack.pending && !state.recovery.inProgress
                && !WeaponTransition::IsUnsettled(a_remote) && !HandPresentation::IsPlaying(a_remote)
                && !IdlePresentation::IsPlaying(a_remote) && !a_swimming;
            if (!reconcilable)
            {
                stance.sampleElapsed = 0.0F;
                stance.sneakMismatchElapsed = 0.0F;
                stance.standingMismatchElapsed = 0.0F;
                return;
            }
            stance.sampleElapsed += FrameTime::Clamp(a_deltaTime);
            if (stance.sampleElapsed < kGraphStanceSampleSeconds)
            {
                return;
            }
            const float sampledSeconds = stance.sampleElapsed;
            stance.sampleElapsed = 0.0F;
            const auto selectors = GraphStance::ReadStanceSelectors(a_actor);

            const auto graphSneak = GraphStance::ReadSneak(a_actor, selectors.sneak);
            const bool targetSneaking = IsSneaking(target.movementState);
            if (graphSneak.sneak != (targetSneaking ? GraphStance::Sneak::kNotSneaking : GraphStance::Sneak::kSneaking))
            {
                stance.sneakMismatchElapsed = 0.0F;
            }
            else
            {
                stance.sneakMismatchElapsed += sampledSeconds;
                if (stance.sneakMismatchElapsed >= kGraphSneakMismatchSeconds
                    && stance.sneakCorrections < kMaximumGraphStanceCorrections)
                {
                    stance.sneakMismatchElapsed = 0.0F;
                    stance.standingMismatchElapsed = 0.0F;
                    ++stance.sneakCorrections;
                    // Latch what the graph shows, so the next dispatch sends the missing
                    // edge through the normal ordering, retries, and post-sneak moveStart.
                    dispatch.latched.movementState = ToggleSneak(target.movementState);
                    logs::debug(
                        "Animation sync graph sneak disagrees with the latched state; reasserting: actor={:08X} "
                        "state={} graphSneak={} iState={} correction={}/{}",
                        a_remote.handle.native_handle(), Multiplayer::GetMovementStateName(target.movementState),
                        GraphStance::GetSneakName(graphSneak), GraphStance::ReadStateTag(a_actor),
                        stance.sneakCorrections, kMaximumGraphStanceCorrections);
                    return;
                }
            }

            // Only standing under a moving target is repaired: the graph rejected
            // moveStop once Speed was zeroed, and the receiver writes Speed 0 every
            // pass while the target is idle.
            if (!IsMoving(target.movementState) || selectors.moving != GraphStance::SelectorState::kOff)
            {
                stance.standingMismatchElapsed = 0.0F;
                return;
            }
            stance.standingMismatchElapsed += sampledSeconds;
            if (stance.standingMismatchElapsed < kGraphStandingMismatchSeconds
                || stance.standingCorrections >= kMaximumGraphStanceCorrections)
            {
                return;
            }
            stance.standingMismatchElapsed = 0.0F;
            ++stance.standingCorrections;
            dispatch.latched.movementState = ToStanding(target.movementState);
            logs::debug(
                "Animation sync graph stands while the latched state moves; reasserting: actor={:08X} state={} "
                "graphMoving={} graphSneak={} iState={} correction={}/{}",
                a_remote.handle.native_handle(), Multiplayer::GetMovementStateName(target.movementState),
                GraphStance::GetMovingName(selectors.moving), GraphStance::GetSneakName(graphSneak),
                GraphStance::ReadStateTag(a_actor), stance.standingCorrections, kMaximumGraphStanceCorrections);
        }

        // Bounds how long translation waits for the graph revision that matches the
        // checkpoint, so a rejected edge does not leave the actor standing still.
        bool HoldTranslationForDispatch(RemoteActor& a_remote, float a_deltaTime)
        {
            auto& state = a_remote.animation;
            auto& hold = state.translationHold;
            if (IsAnimationStateSettledExceptLanding(a_remote))
            {
                hold = {};
                return false;
            }

            hold.elapsed += FrameTime::Clamp(a_deltaTime);
            if (hold.elapsed < kUnsettledTranslationHoldSeconds)
            {
                return true;
            }
            if (!hold.reported)
            {
                hold.reported = true;
                logs::debug(
                    "Animation sync resumed translation while its locomotion dispatch is unsettled: actor={:08X} "
                    "dispatched={} heldSeconds={:.3f} rejectedEventAttempts={}",
                    a_remote.handle.native_handle(),
                    Multiplayer::GetMovementStateName(state.dispatch.latched.movementState), hold.elapsed,
                    state.dispatch.eventAttempts);
                if (auto actor = a_remote.handle.get())
                {
                    LogGraphProbe(*actor, a_remote, "unsettled-translation");
                }
            }
            return false;
        }
    }

    Multiplayer::AnimationState GetTargetState(const RemoteActor& a_remote)
    {
        return a_remote.animation.targetAnimationState;
    }

    void SetAnimationState(RemoteActor& a_remote, Multiplayer::AnimationState a_animationState)
    {
        if (a_remote.form != Multiplayer::Shapeshift::kNone && IsSneaking(a_animationState.movementState))
        {
            // The beast graphs take no SneakStart: a sneaking beast walks or runs as usual.
            a_animationState.movementState = ToggleSneak(a_animationState.movementState);
        }
        auto& state = a_remote.animation;
        const auto previousAnimationState = state.targetAnimationState;
        state.targetAnimationState = a_animationState;
        ActorRegistry::SetAuthoritativeJumpState(a_remote, a_animationState.jumping);
        ActorRegistry::SetAuthoritativeDirection(a_remote);
        ActorRegistry::SetAuthoritativeWeaponDrawnState(a_remote, a_animationState.weaponDrawn);
        RequestAnimationUpdate(state);
        if (previousAnimationState != a_animationState)
        {
            state.stance.sneakCorrections = 0;
            state.stance.standingCorrections = 0;
            logs::debug("Animation sync updated actor request: actor={:08X} revision={} previous={}/{} desired={}/{}",
                a_remote.handle.native_handle(), static_cast<unsigned long long>(state.requestedRevision),
                Multiplayer::GetMovementStateName(previousAnimationState.movementState),
                GetJumpName(previousAnimationState.jumping),
                Multiplayer::GetMovementStateName(a_animationState.movementState),
                GetJumpName(a_animationState.jumping));
        }
    }

    bool SendNetworkAnimationEvent(RemoteActor& a_remote, const RE::BSFixedString& a_eventName)
    {
        PROFILE_FUNCTION();
        if (!GameThread::Require("SendNetworkAnimationEvent"))
        {
            return false;
        }

        auto actorPtr = a_remote.handle.get();
        if (!actorPtr || !a_remote.animationGraphHolder)
        {
            return false;
        }

        auto* actor = actorPtr.get();
        auto* holder = static_cast<RE::IAnimationGraphManagerHolder*>(actor);
        if (holder != a_remote.animationGraphHolder)
        {
            return false;
        }

        return AnimationFirewall::NotifyNetworkAnimationGraph(*actor, a_eventName);
    }

    bool IsAnimationStateSettled(const RemoteActor& a_remote)
    {
        return !a_remote.animation.landing.completionPending && IsAnimationStateSettledExceptLanding(a_remote);
    }

    bool IsAnimationStateSettledExceptLanding(const RemoteActor& a_remote)
    {
        return a_remote.animation.processedRevision == a_remote.animation.requestedRevision;
    }

    const RE::ActorState* GetCombatReadyState(const RemoteActor& a_remote, const RE::Actor& a_actor)
    {
        const auto& state = a_remote.animation;
        const auto& target = state.targetAnimationState;
        if (!a_remote.ready || !a_remote.animationGraphHolder || !IsAnimationStateSettled(a_remote)
            || state.recovery.inProgress || target.jumping || !target.weaponDrawn)
        {
            return nullptr;
        }

        const auto* holder = static_cast<const RE::IAnimationGraphManagerHolder*>(std::addressof(a_actor));
        const auto* actorState = a_actor.AsActorState();
        if (holder != a_remote.animationGraphHolder || !actorState
            || actorState->GetWeaponState() != RE::WEAPON_STATE::kDrawn)
        {
            return nullptr;
        }

        RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
        const bool trackedGraph = a_actor.Is3DLoaded() && a_actor.GetAnimationGraphManager(graphManager)
            && graphManager && state.dispatch.graph == graphManager.get();
        return trackedGraph ? actorState : nullptr;
    }

    bool IsSwimming(const RemoteActor& a_remote)
    {
        if (!GameThread::Require("IsSwimming"))
        {
            return false;
        }
        const auto actorPtr = a_remote.handle.get();
        const auto* actorState = actorPtr ? actorPtr->AsActorState() : nullptr;
        return actorState && actorState->IsSwimming();
    }

    void RequestAnimationUpdate(State& a_state)
    {
        ++a_state.requestedRevision;
    }

    void RequestCheckpointUpdate(State& a_state)
    {
        a_state.checkpointUpdatePending = true;
    }

    void InvalidateAnimationDispatch(State& a_state)
    {
        a_state.dispatch = {};
        a_state.landing = {};
        a_state.weapon = {};
        a_state.swim = {};
        a_state.stance = {};
        a_state.attack = {};
        a_state.hands = {};
        a_state.idle = {};
        a_state.direction = {};
        a_state.processedRevision = 0;
    }

    void HoldIdleTurnState(RE::Actor& a_actor, RemoteActor& a_remote)
    {
        // Skyrim records the local AI's turn-in-place request in iSyncTurnState along
        // with its TurnLeft/TurnRight/turnStop events. The remote actor's AI keeps asking
        // to face its own target because the network owns the heading. The firewall
        // blocks those events, but every idle turn state machine takes this variable
        // as its start state. Each time one reactivates (after a weapon draw or sheathe,
        // IdleForceDefaultState, or stopping), it would enter a turn clip that it can
        // leave only on the blocked turnStop, looping until a network event leaves idle.
        static const RE::BSFixedString syncTurnStateVariable{ "iSyncTurnState" };
        std::int32_t syncTurnState = kStandingTurnState;
        if (!a_actor.GetGraphVariableInt(syncTurnStateVariable, syncTurnState)
            || syncTurnState == kStandingTurnState)
        {
            return;
        }

        const bool accepted = a_actor.SetGraphVariableInt(syncTurnStateVariable, kStandingTurnState);
        // The AI may have written the variable after the previous pass, just as a
        // machine reactivated. turnStop returns any turn state to standing, and a
        // standing state has no turnStop transition.
        static const RE::BSFixedString turnStopEvent{ "turnStop" };
        const bool turnStopAccepted = SendNetworkAnimationEvent(a_remote, turnStopEvent);
        auto& state = a_remote.animation;
        ++state.turnStateOverrides;
        if (ActorConstants::ShouldLogOccurrence(state.turnStateOverrides))
        {
            logs::debug(
                "Animation sync held idle turn state: actor={:08X} localTurnState={} accepted={} turnStop={} count={}",
                a_remote.handle.native_handle(), syncTurnState, accepted, turnStopAccepted, state.turnStateOverrides);
        }
    }

    void ApplyAnimationUpdate(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime)
    {
        PROFILE_FUNCTION();
        const auto landingSinkRegistered = ObserveGraph(a_actor, a_remote);
        if (!landingSinkRegistered)
        {
            return;
        }

        auto& state = a_remote.animation;
        const auto* actorState = a_actor.AsActorState();
        const bool swimming = actorState && actorState->IsSwimming();
        AdvanceLanding(a_actor, a_remote, swimming, a_deltaTime);
        IdlePresentation::Advance(a_actor, a_remote, a_deltaTime);
        if (std::exchange(state.checkpointUpdatePending, false))
        {
            RequestAnimationUpdate(state);
        }
        const auto weaponTransition = WeaponTransition::Advance(a_actor, a_remote, a_deltaTime);
        if (weaponTransition.settling || IdlePresentation::ConsumeReleaseSettle(a_remote))
        {
            return;
        }
        AdvanceSwim(a_actor, a_remote, swimming, a_deltaTime);

        const auto plan = PlanDispatch(
            a_actor, a_remote, swimming, weaponTransition.completed, *landingSinkRegistered, a_deltaTime);
        if (!plan)
        {
            return;
        }
        if (plan->jumpRequired)
        {
            // JumpLand starts a timed landing branch whose graph output emits
            // JumpLandEnd later, often from an animation worker thread. Observe that
            // output only while the network-authoritative state is grounded.
            ActorRegistry::SetLandingCompletionObservation(a_actor, plan->jumpLandRequired);
        }
        const auto stance = ReadStanceForEdges(a_actor, a_remote, *plan);
        EdgeResults edges;
        if (!SendStopEdges(a_actor, a_remote, *plan, stance, edges, a_deltaTime))
        {
            return;
        }
        if (plan->jumpLandRequired)
        {
            AlignLanding(a_actor, a_remote);
        }
        const auto rejectedVariables = WriteVariables(a_actor, a_remote, *plan);
        const bool variablesAccepted = (rejectedVariables & kRequiredVariables) == 0;
        if (plan->RequiresEvents())
        {
            DispatchEdges(a_actor, a_remote, *plan, stance, rejectedVariables, edges, a_deltaTime);
        }
        else if (variablesAccepted)
        {
            state.processedRevision = plan->revision;
        }
        AdvanceDirectionBlend(a_remote, plan->state, variablesAccepted, a_deltaTime);

        // Let the native transition start only after this checkpoint's persistent
        // speed and locomotion events have settled.
        if (IsAnimationStateSettled(a_remote))
        {
            WeaponTransition::RequestNativeTransition(a_actor, state.targetAnimationState.weaponDrawn);
        }
        ReconcileGraphStance(a_actor, a_remote, swimming, a_deltaTime);
        AdvanceGraphProbe(a_actor, a_remote, a_deltaTime);
    }

    std::optional<ActorMovement::TranslationMode> GetTranslationMode(
        RemoteActor& a_remote, const std::optional<Multiplayer::AnimationState>& a_presentable, float a_deltaTime)
    {
        using ActorMovement::TranslationMode;
        if (HoldTranslationForDispatch(a_remote, a_deltaTime))
        {
            return std::nullopt;
        }

        const auto& state = a_remote.animation;
        const auto& target = state.targetAnimationState;
        const bool attackPlaying = state.attack.pending;
        const auto getMode = [attackPlaying](const Multiplayer::AnimationState& a_followed) {
            if (a_followed.jumping)
            {
                return TranslationMode::kAirborne;
            }
            if (attackPlaying)
            {
                return TranslationMode::kAttack;
            }
            return IsMoving(a_followed.movementState) ? TranslationMode::kMoving : TranslationMode::kStanding;
        };
        if (a_presentable && (*a_presentable == target || target.jumping))
        {
            return getMode(target);
        }
        if (a_presentable && attackPlaying)
        {
            return getMode(*a_presentable);
        }
        // Furniture owns the actor, or the target is a staged step toward the
        // presentable state: translating would pair the latest position with stale
        // graph state. A pending landing keeps following the pose.
        if (state.landing.completionPending)
        {
            return getMode(target);
        }
        return std::nullopt;
    }

    bool IsFurnitureEntryBlocked(const RemoteActor& a_remote)
    {
        return a_remote.animation.attack.pending || HandPresentation::IsPlaying(a_remote)
            || WeaponTransition::IsUnsettled(a_remote) || IdlePresentation::IsPlaying(a_remote)
            || a_remote.animation.recovery.inProgress;
    }
}
