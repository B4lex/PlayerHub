#include "sync/idle/IdleSync.h"
#include "actor/ActorConstants.h"
#include "actor/GraphStance.h"
#include "actor/IdlePresentation.h"
#include "actor/RemoteActor.h"
#include "network/ServerClient.h"
#include "runtime/FrameTime.h"
#include "runtime/Profiling.h"
#include "text/AsciiCase.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace IdleSync
{
    namespace
    {
        using Multiplayer::IsMoving;
        using Multiplayer::IsSneaking;

        constexpr std::size_t kMaximumQueuedIdles = 32;
        constexpr float kIdleActionTimeoutSeconds = 3.0F;
        constexpr std::uint32_t kMaximumIdleAttempts = 4;
        constexpr float kMinimumIdleRetrySeconds = 0.25F;
        // Idle actions share the server's action allowance (10 a second, burst 10) with
        // attacks, and an action it drops leaves a sequence gap that rejects the
        // player's later actions, so a script that floods idle events is cut here.
        constexpr float kLocalIdlesPerSecond = 2.0F;
        constexpr float kLocalIdleBurst = 4.0F;
        constexpr float kIdleSampleSeconds = 0.1F;
        // The idle machine switches state on the graph update after the event, so
        // the default state read right after it does not end the idle.
        constexpr float kIdleEntrySeconds = 0.5F;
        // A held idle's anchor checkpoint plays a playout delay after its action
        // arrives; this covers an action that arrives later anyway.
        constexpr float kHeldEntryGraceSeconds = 0.3F;
        // Longer than the actor's idle trails its source's, so an idle that ends on
        // both graphs by itself is not cut short.
        constexpr float kHeldEndReleaseSeconds = 1.0F;

        // Sent to the player's graph by the engine: the reset that ends every idle,
        // and the idle machine's return to its initial state at furniture entry.
        constexpr bool IsSystemIdleEvent(std::string_view a_name) noexcept
        {
            return AsciiCase::Equals(a_name, "IdleForceDefaultState") || AsciiCase::Equals(a_name, "IdlePlayer");
        }

        constexpr bool IsCapturedEvent(std::string_view a_name) noexcept
        {
            return Multiplayer::IsValidIdleActionEvent(a_name) && !IsSystemIdleEvent(a_name);
        }

        // The vanilla exits: IdleChairExitStart, IdleRailLeanExit, IdleShackled_01_exit,
        // IdleStop, IdleStopInstant, IdleOffsetStop, IdleMesmerizeStop, OffsetStop.
        constexpr bool IsExitEvent(std::string_view a_name) noexcept
        {
            return AsciiCase::Contains(a_name, "Exit") || AsciiCase::EndsWith(a_name, "Stop")
                || AsciiCase::EndsWith(a_name, "StopInstant");
        }

        static_assert(IsCapturedEvent("IdleLaugh") && IsCapturedEvent("offsetStop"));
        static_assert(!IsCapturedEvent("IdleForceDefaultState") && !IsCapturedEvent("idleplayer"));
        static_assert(!IsCapturedEvent("unequip") && !IsCapturedEvent("OffsetCarryBucketStart"));
        static_assert(IsExitEvent("IdleChairExitStart") && IsExitEvent("IdleRailLeanExit"));
        static_assert(IsExitEvent("IdleShackled_01_exit") && IsExitEvent("IdleStopInstant"));
        static_assert(IsExitEvent("IdleStop") && IsExitEvent("OffsetStop"));
        static_assert(!IsExitEvent("IdleSitCrossLeggedEnter") && !IsExitEvent("IdleQuickStart"));

        // A change of the source's state that its idle does not outlast: a start of
        // movement, a jump, a draw or sheathe, or a sneak toggle. A stop does not end
        // it, since the source's published stop can trail the idle's start.
        constexpr const char* FindInterruption(
            const Multiplayer::AnimationState& a_previous, const Multiplayer::AnimationState& a_current) noexcept
        {
            if (!IsMoving(a_previous.movementState) && IsMoving(a_current.movementState))
            {
                return "move";
            }
            if (!a_previous.jumping && a_current.jumping)
            {
                return "jump";
            }
            if (a_previous.weaponDrawn != a_current.weaponDrawn)
            {
                return "weapon";
            }
            if (IsSneaking(a_previous.movementState) != IsSneaking(a_current.movementState))
            {
                return "sneak";
            }
            return nullptr;
        }

        struct CapturedIdle
        {
            std::array<char, Multiplayer::kMaximumIdleEventLength> name{};
            std::size_t length{ 0 };

            [[nodiscard]] std::string_view View() const noexcept { return { name.data(), length }; }
        };

        struct LocalHeld
        {
            Multiplayer::IdleEvent event;
            Multiplayer::AnimationState previous{};
            bool hasPrevious{ false };
            bool observedIdle{ false };
            float elapsed{ 0.0F };
            float sampleElapsed{ 0.0F };
        };

        // The graph input hook captures on any thread while the local player is human.
        std::atomic<bool> s_captureOpen{ false };
        std::mutex s_capturedLock;
        std::array<CapturedIdle, kMaximumQueuedIdles> s_captured{};
        std::size_t s_capturedCount = 0;

        std::array<CapturedIdle, kMaximumQueuedIdles> s_drained{};
        std::vector<Multiplayer::IdleEvent> s_pending;
        LocalHeld s_held;
        float s_rateTokens = kLocalIdleBurst;
        // Only throttle the log.
        std::uint32_t s_droppedOverRate = 0;
        std::uint32_t s_ignoredExits = 0;

        const char* FindLocalBlock(RE::PlayerCharacter& a_player)
        {
            const auto* actorState = a_player.AsActorState();
            if (!actorState)
            {
                return "no-actor-state";
            }
            // Not the sit/sleep state: idles in the unarmed root's FurnitureState, such as
            // sitting cross-legged, set it without occupying anything (observed in game),
            // and real furniture is occupied from its activation, before its entry idle.
            if (a_player.GetOccupiedFurniture())
            {
                return "furniture";
            }
            if (a_player.IsOnMount())
            {
                return "mount";
            }
            if (a_player.IsDead())
            {
                return "dead";
            }
            if (actorState->IsSwimming())
            {
                return "swimming";
            }
            if (a_player.IsInMidair())
            {
                return "midair";
            }
            return nullptr;
        }

        void EndLocalHeld(const char* a_reason)
        {
            if (!s_held.event)
            {
                return;
            }
            logs::debug("Idle sync ended local held idle: event={} reason={} seconds={:.3f} observedIdle={}",
                *s_held.event, a_reason, s_held.elapsed, s_held.observedIdle);
            s_held = {};
        }

        void QueueLocalIdle(std::string_view a_event, bool a_exit)
        {
            if (s_rateTokens < 1.0F)
            {
                ++s_droppedOverRate;
                if (ActorConstants::ShouldLogOccurrence(s_droppedOverRate))
                {
                    logs::warn("Idle sync dropped local idle event over the rate limit: event={} count={}", a_event,
                        s_droppedOverRate);
                }
                return;
            }
            if (s_pending.size() >= kMaximumQueuedIdles)
            {
                return;
            }
            s_rateTokens -= 1.0F;
            logs::debug("Idle sync captured local idle event: event={} exit={}", a_event, a_exit);
            s_pending.push_back(std::make_shared<const std::string>(a_event));
        }

        void ObserveLocalHeld(RE::PlayerCharacter& a_player, float a_delta)
        {
            s_held.elapsed += a_delta;
            s_held.sampleElapsed += a_delta;
            if (s_held.sampleElapsed < kIdleSampleSeconds)
            {
                return;
            }
            s_held.sampleElapsed = 0.0F;
            // A graph without the vanilla idle machines reads kNone, which keeps the idle
            // until an exit or a state change ends it.
            const auto reading = GraphStance::ReadStanceSelectors(a_player).idle;
            if (reading == GraphStance::SelectorState::kOn)
            {
                s_held.observedIdle = true;
            }
            else if (reading == GraphStance::SelectorState::kOff
                && (s_held.observedIdle || s_held.elapsed >= kIdleEntrySeconds))
            {
                EndLocalHeld(s_held.observedIdle ? "graph-default" : "graph-unobserved");
            }
        }

        void LogRejected(const RemoteFrame& a_remote, RemoteState& a_state, const RemoteActor& a_actor,
            const RE::BSFixedString& a_event, std::uint32_t a_attempts, const char* a_source)
        {
            ++a_state.rejectedEvents;
            if (ActorConstants::ShouldLogOccurrence(a_state.rejectedEvents))
            {
                logs::warn("Idle sync remote graph rejected idle event: player={} actor={:08X} event={} source={} "
                           "attempts={} count={}",
                    a_remote.playerID, a_actor.handle.native_handle(), a_event.c_str(), a_source, a_attempts,
                    a_state.rejectedEvents);
            }
        }

        void DispatchQueued(const RemoteFrame& a_remote, RemoteState& a_state, RemoteActor& a_actor, bool a_mayStart)
        {
            const auto& checkpoint = a_remote.checkpoint;
            auto& queue = a_state.pendingIdles;
            auto& pending = queue.front();
            const auto& action = pending.action;
            if (action.graphEpoch != checkpoint.graphEpoch || action.checkpointSequence > checkpoint.sequence)
            {
                return;
            }
            if (pending.exit && !IdlePresentation::IsPlaying(a_actor))
            {
                logs::debug("Idle sync dropped idle exit without a playing idle: player={} event={} actionSequence={}",
                    a_remote.playerID, pending.event.c_str(), static_cast<unsigned long long>(action.actionSequence));
                queue.pop_front();
                return;
            }
            if (!a_mayStart || (pending.dispatchAttempts != 0 && pending.retryElapsed < kMinimumIdleRetrySeconds))
            {
                return;
            }

            const bool accepted = IdlePresentation::Dispatch(a_actor, pending.event, pending.exit);
            ++pending.dispatchAttempts;
            pending.retryElapsed = 0.0F;
            const bool entersHeld = !pending.exit && pending.event == a_state.held;
            if (accepted)
            {
                if (pending.exit)
                {
                    a_state.endedInstance = 0;
                }
                logs::debug("Idle sync processed idle action: player={} actor={:08X} event={} exit={} "
                            "actionSequence={} attempts={} queued={}",
                    a_remote.playerID, a_actor.handle.native_handle(), pending.event.c_str(), pending.exit,
                    static_cast<unsigned long long>(action.actionSequence), pending.dispatchAttempts,
                    queue.size() - 1);
            }
            else if (pending.dispatchAttempts >= kMaximumIdleAttempts)
            {
                LogRejected(a_remote, a_state, a_actor, pending.event, pending.dispatchAttempts, "action");
            }
            else
            {
                return;
            }
            if (entersHeld)
            {
                // Rejected too: the state's re-entry would only be rejected again.
                IdlePresentation::SetAppliedInstance(a_actor, a_state.heldInstance);
                a_state.heldAttempts = 0;
            }
            queue.pop_front();
        }

        void ReenterHeld(const RemoteFrame& a_remote, RemoteState& a_state, RemoteActor& a_actor, bool a_mayStart)
        {
            if (a_state.held.empty() || a_state.heldInstance == IdlePresentation::GetAppliedInstance(a_actor)
                || a_state.heldElapsed < kHeldEntryGraceSeconds || !a_mayStart
                || (a_state.heldAttempts != 0 && a_state.heldRetryElapsed < kMinimumIdleRetrySeconds))
            {
                return;
            }
            const bool accepted = IdlePresentation::Dispatch(a_actor, a_state.held, false);
            ++a_state.heldAttempts;
            a_state.heldRetryElapsed = 0.0F;
            if (accepted)
            {
                logs::debug("Idle sync re-entered held idle from state: player={} actor={:08X} event={} instance={} "
                            "attempts={}",
                    a_remote.playerID, a_actor.handle.native_handle(), a_state.held.c_str(),
                    static_cast<unsigned long long>(a_state.heldInstance), a_state.heldAttempts);
            }
            else if (a_state.heldAttempts >= kMaximumIdleAttempts)
            {
                LogRejected(a_remote, a_state, a_actor, a_state.held, a_state.heldAttempts, "state");
            }
            else
            {
                return;
            }
            IdlePresentation::SetAppliedInstance(a_actor, a_state.heldInstance);
            a_state.heldAttempts = 0;
        }
    }

    void ObserveLocalGraphEvent(const RE::BSFixedString& a_eventName, bool a_modded)
    {
        // Modded idles reach peers as poses (Remote actor, Skeleton pose).
        if (a_modded || !s_captureOpen.load(std::memory_order_acquire))
        {
            return;
        }
        const std::string_view name{ a_eventName };
        if (!IsCapturedEvent(name))
        {
            return;
        }
        const std::scoped_lock lock{ s_capturedLock };
        if (s_capturedCount < s_captured.size())
        {
            auto& captured = s_captured[s_capturedCount++];
            std::memcpy(captured.name.data(), name.data(), name.size());
            captured.length = name.size();
        }
    }

    void QueueLocalIdles(
        const LocalFrame& a_local, bool a_transformed, bool a_poseWindowOpened, bool a_graphEpochChanged, float a_delta)
    {
        PROFILE_FUNCTION();
        s_captureOpen.store(!a_transformed, std::memory_order_release);
        std::size_t drained = 0;
        {
            const std::scoped_lock lock{ s_capturedLock };
            drained = s_capturedCount;
            std::copy_n(s_captured.begin(), drained, s_drained.begin());
            s_capturedCount = 0;
        }
        const float delta = FrameTime::Clamp(a_delta);
        s_rateTokens = (std::min)(s_rateTokens + delta * kLocalIdlesPerSecond, kLocalIdleBurst);
        if (a_graphEpochChanged)
        {
            // An idle of the previous graph cannot be attached to a checkpoint of this one.
            s_pending.clear();
            EndLocalHeld("graph-epoch");
        }
        if (a_poseWindowOpened)
        {
            EndLocalHeld("pose-window");
        }

        const auto* blocked = a_transformed ? "shapeshift" : FindLocalBlock(a_local.player);
        for (std::size_t index = 0; index < drained; ++index)
        {
            auto name = s_drained[index].View();
            if (blocked)
            {
                // Furniture, mounts, and beast forms play their own idles natively on peers.
                logs::debug("Idle sync ignored local idle event: event={} reason={}", name, blocked);
                continue;
            }
            if (!IsExitEvent(name))
            {
                QueueLocalIdle(name, false);
                s_held = LocalHeld{ .event = std::make_shared<const std::string>(name) };
                logs::debug("Idle sync holding local idle: event={}", name);
                continue;
            }
            if (!s_held.event)
            {
                // The engine and scripts also stop idles that peers never received.
                if (ActorConstants::ShouldLogOccurrence(++s_ignoredExits))
                {
                    logs::debug("Idle sync ignored local idle exit without a held idle: event={} count={}", name,
                        s_ignoredExits);
                }
                continue;
            }
            if (AsciiCase::Equals(name, Multiplayer::kOffsetStopEvent))
            {
                name = Multiplayer::kOffsetStopEvent;
            }
            QueueLocalIdle(name, true);
            EndLocalHeld("exit");
        }
        if (blocked)
        {
            EndLocalHeld(blocked);
        }
        else if (s_held.event)
        {
            ObserveLocalHeld(a_local.player, delta);
        }
    }

    void DiscardLocalIdles()
    {
        s_pending.clear();
        const std::scoped_lock lock{ s_capturedLock };
        s_capturedCount = 0;
    }

    Multiplayer::IdleEvent SampleLocalIdle(const LocalSample& a_sample)
    {
        if (!s_held.event)
        {
            return {};
        }
        if (a_sample.furniture || a_sample.mounted || a_sample.dead || a_sample.swimming || a_sample.transformed)
        {
            EndLocalHeld("checkpoint-state");
            return {};
        }
        if (s_held.hasPrevious)
        {
            if (const auto* reason = FindInterruption(s_held.previous, a_sample.animationState))
            {
                EndLocalHeld(reason);
                return {};
            }
        }
        s_held.previous = a_sample.animationState;
        s_held.hasPrevious = true;
        return s_held.event;
    }

    void PublishLocalIdles(const Network::PlayerTransform& a_checkpoint)
    {
        if (a_checkpoint.furniture.IsEmpty() && a_checkpoint.shapeshift == Multiplayer::Shapeshift::kNone)
        {
            for (const auto& event : s_pending)
            {
                Network::PublishLocalIdleAction(*event, a_checkpoint.graphEpoch, a_checkpoint.sequence);
            }
        }
        s_pending.clear();
    }

    void OnCheckpointPlayed(RemoteState& a_state, const CheckpointUpdate& a_update)
    {
        const auto& checkpoint = a_update.checkpoint;
        if (!a_update.inserted)
        {
            const auto& previous = a_update.previous;
            if (previous.graphEpoch != checkpoint.graphEpoch)
            {
                const auto newEpoch = checkpoint.graphEpoch;
                const auto removed = std::erase_if(a_state.pendingIdles,
                    [newEpoch](const PendingIdle& a_pending) { return a_pending.action.graphEpoch != newEpoch; });
                if (removed != 0)
                {
                    logs::debug("Idle sync rejected queued idles at graph-epoch boundary: player={} graphEpoch={} "
                                "rejected={}",
                        a_update.playerID, static_cast<unsigned long long>(newEpoch), removed);
                }
            }
            const auto* interruption = FindInterruption(previous.animationState, checkpoint.animationState);
            if (!interruption && !previous.swimming && checkpoint.swimming)
            {
                interruption = "swim";
            }
            if (!interruption && previous.furniture.IsEmpty() && !checkpoint.furniture.IsEmpty())
            {
                interruption = "furniture";
            }
            if (interruption)
            {
                a_state.interruption = interruption;
                a_state.interruptionSequence = checkpoint.sequence;
            }
        }
        if (a_state.heldEvent == checkpoint.idle)
        {
            return;
        }
        const bool changed = !Multiplayer::IsSameIdleEvent(a_state.heldEvent, checkpoint.idle);
        a_state.heldEvent = checkpoint.idle;
        if (!changed)
        {
            return;
        }
        if (a_state.heldInstance != 0)
        {
            a_state.endedInstance = a_state.heldInstance;
            a_state.endedElapsed = 0.0F;
        }
        a_state.held = checkpoint.idle ? RE::BSFixedString{ checkpoint.idle->c_str() } : RE::BSFixedString{};
        a_state.heldInstance = checkpoint.idle ? checkpoint.sequence : 0;
        a_state.heldElapsed = 0.0F;
        a_state.heldAttempts = 0;
        a_state.heldRetryElapsed = 0.0F;
        logs::debug("Idle sync remote held idle changed: player={} idle={} sequence={}", a_update.playerID,
            checkpoint.idle ? std::string_view{ *checkpoint.idle } : std::string_view{ "-" },
            static_cast<unsigned long long>(checkpoint.sequence));
    }

    void AcceptRemoteAction(
        RemoteState* a_state, const Network::PlayerTransform* a_checkpoint, Network::AnimationAction&& a_action)
    {
        if (!a_state || !a_checkpoint || !a_action.idle || a_action.graphEpoch < a_checkpoint->graphEpoch
            || !a_checkpoint->furniture.IsEmpty())
        {
            logs::debug("Idle sync rejected idle action without a compatible checkpoint: player={} actionSequence={} "
                        "graphEpoch={}",
                a_action.playerID, static_cast<unsigned long long>(a_action.actionSequence),
                static_cast<unsigned long long>(a_action.graphEpoch));
            return;
        }
        if (a_state->pendingIdles.size() >= kMaximumQueuedIdles)
        {
            logs::warn("Idle sync rejected idle action because the bounded queue is full: player={} actionSequence={}",
                a_action.playerID, static_cast<unsigned long long>(a_action.actionSequence));
            return;
        }
        RE::BSFixedString event{ a_action.idle->c_str() };
        const bool exit = IsExitEvent(*a_action.idle);
        a_state->pendingIdles.push_back(
            PendingIdle{ .action = std::move(a_action), .event = std::move(event), .exit = exit });
    }

    void AgeRemoteActions(const RemoteFrame& a_remote, RemoteState& a_state, float a_delta)
    {
        const float delta = FrameTime::Clamp(a_delta);
        auto& queue = a_state.pendingIdles;
        for (auto& pending : queue)
        {
            pending.age += delta;
            if (pending.dispatchAttempts != 0)
            {
                pending.retryElapsed += delta;
            }
        }
        if (!a_state.held.empty())
        {
            a_state.heldElapsed += delta;
            a_state.heldRetryElapsed += delta;
        }
        if (a_state.endedInstance != 0)
        {
            a_state.endedElapsed += delta;
        }
        while (!queue.empty() && queue.front().age >= kIdleActionTimeoutSeconds)
        {
            const auto& pending = queue.front();
            logs::warn("Idle sync timed out queued idle action: player={} event={} actionSequence={}",
                a_remote.playerID, pending.event.c_str(),
                static_cast<unsigned long long>(pending.action.actionSequence));
            queue.pop_front();
        }
    }

    void ClearRemoteActions(RemoteState& a_state)
    {
        a_state.pendingIdles.clear();
        a_state.interruption = nullptr;
    }

    void SyncIdle(const RemoteFrame& a_remote, RemoteState& a_state, RemoteActor& a_actor, bool a_handsSettled,
        bool a_attackQueued)
    {
        PROFILE_FUNCTION();
        const auto& checkpoint = a_remote.checkpoint;
        const auto& hands = checkpoint.handPresentation;
        const bool combatPending
            = a_attackQueued || hands.blocking || hands.bowDrawn || hands.castingLeft || hands.castingRight;
        if (combatPending)
        {
            // An idle state takes no attack, block, bow, or cast event, and the source
            // left its idle to start one.
            IdlePresentation::Release(a_actor, a_attackQueued ? "attack" : "hands");
        }
        if (a_state.interruption || !checkpoint.furniture.IsEmpty())
        {
            const auto* reason = a_state.interruption ? a_state.interruption : "furniture";
            IdlePresentation::Release(a_actor, reason);
            // An idle the source left before the actor could start it is history.
            const auto sequence = a_state.interruption ? a_state.interruptionSequence : checkpoint.sequence;
            const auto removed = std::erase_if(a_state.pendingIdles, [sequence](const PendingIdle& a_pending) {
                return !a_pending.exit && a_pending.action.checkpointSequence <= sequence;
            });
            if (removed != 0)
            {
                logs::debug("Idle sync dropped queued idles the source left: player={} reason={} dropped={}",
                    a_remote.playerID, reason, removed);
            }
            a_state.interruption = nullptr;
        }
        if (a_state.endedInstance != 0 && a_state.endedElapsed >= kHeldEndReleaseSeconds
            && std::ranges::none_of(a_state.pendingIdles, &PendingIdle::exit))
        {
            // The source left the idle the actor was given without an exit the actor
            // played: its graph returned to default through events peers never get, or
            // the exit timed out in the queue.
            if (IdlePresentation::GetAppliedInstance(a_actor) == a_state.endedInstance)
            {
                IdlePresentation::Release(a_actor, "held-ended");
            }
            a_state.endedInstance = 0;
        }
        if (a_state.pendingIdles.empty() && a_state.held.empty())
        {
            return;
        }

        auto actor = a_actor.handle.get();
        if (!actor)
        {
            return;
        }
        const bool mayStart = a_handsSettled && !combatPending && checkpoint.furniture.IsEmpty() && !checkpoint.swimming
            && !checkpoint.animationState.jumping
            && IdlePresentation::CanStart(a_actor, *actor, checkpoint.animationState.weaponDrawn);
        // One event per pass, so a burst does not stack edges in one graph update.
        if (!a_state.pendingIdles.empty())
        {
            DispatchQueued(a_remote, a_state, a_actor, mayStart);
            return;
        }
        ReenterHeld(a_remote, a_state, a_actor, mayStart);
    }
}
