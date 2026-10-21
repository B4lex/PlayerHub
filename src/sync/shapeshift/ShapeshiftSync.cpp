#include "sync/shapeshift/ShapeshiftSync.h"
#include "actor/ActorShapeshift.h"
#include "actor/BeastActions.h"
#include "actor/GraphStance.h"
#include "actor/RemoteActor.h"
#include "forms/BeastForms.h"
#include "network/ServerClient.h"
#include "runtime/FrameTime.h"
#include "runtime/Profiling.h"

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace ShapeshiftSync
{
    namespace
    {
        using Multiplayer::BeastAction;
        using Multiplayer::GetBeastActionGraph;
        using Multiplayer::GetBeastGraph;
        using Multiplayer::Shapeshift;

        constexpr std::size_t kMaximumQueuedActions = 32;
        constexpr float kActionTimeoutSeconds = 3.0F;
        constexpr std::uint32_t kMaximumActionAttempts = 4;
        constexpr float kMinimumActionRetrySeconds = 0.25F;

        // Read by the graph input hook on any thread.
        std::atomic<Shapeshift> s_localForm{ Shapeshift::kNone };
        std::mutex s_capturedLock;
        std::vector<BeastAction> s_captured;
        std::vector<BeastAction> s_pending;

        const char* GetEventName(BeastAction a_action)
        {
            const auto* name = BeastActions::GetEventName(a_action).c_str();
            return name && *name ? name : "<none>";
        }
    }

    Shapeshift ObserveLocalForm(const LocalFrame& a_local)
    {
        const auto form = Forms::GetShapeshift(a_local.player.GetRace());
        if (const auto previous = s_localForm.exchange(form, std::memory_order_acq_rel); previous != form)
        {
            logs::debug("Shapeshift sync local form changed: {} -> {}", Multiplayer::GetShapeshiftName(previous),
                Multiplayer::GetShapeshiftName(form));
        }
        return form;
    }

    Shapeshift GetLocalForm()
    {
        return s_localForm.load(std::memory_order_acquire);
    }

    VampireLordState SampleLocalVampireLord(const LocalFrame& a_local)
    {
        if (GetLocalForm() != Shapeshift::kVampireLord)
        {
            return {};
        }
        const auto& mistForm = Forms::GetVampireLordPower(Forms::VampireLordPower::kMistForm);
        const auto& bats = Forms::GetVampireLordPower(Forms::VampireLordPower::kBats);
        VampireLordState state{ .levitating = GraphStance::ReadStanceSelectors(a_local.player).levitating
            == GraphStance::SelectorState::kOn };
        auto* magicTarget = a_local.player.AsMagicTarget();
        auto* activeEffects = magicTarget ? magicTarget->GetActiveEffectList() : nullptr;
        if (!activeEffects)
        {
            return state;
        }
        for (auto* activeEffect : *activeEffects)
        {
            if (!activeEffect
                || activeEffect->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled))
            {
                continue;
            }
            const auto* effect = activeEffect->GetBaseObject();
            state.mistForm = state.mistForm || (effect && effect == mistForm.effect);
            state.bats = state.bats || (effect && effect == bats.effect);
        }
        return state;
    }

    void ObserveLocalGraphEvent(const RE::BSFixedString& a_eventName)
    {
        const auto form = s_localForm.load(std::memory_order_acquire);
        if (form == Shapeshift::kNone)
        {
            return;
        }
        const auto action = BeastActions::Find(GetBeastGraph(form), a_eventName);
        if (!action)
        {
            return;
        }
        const std::scoped_lock lock{ s_capturedLock };
        if (s_captured.size() < kMaximumQueuedActions)
        {
            s_captured.push_back(*action);
        }
    }

    void QueueLocalActions(bool a_graphEpochChanged)
    {
        std::vector<BeastAction> captured;
        {
            const std::scoped_lock lock{ s_capturedLock };
            captured.swap(s_captured);
        }
        if (a_graphEpochChanged)
        {
            s_pending.clear();
        }
        for (const auto action : captured)
        {
            logs::debug("Shapeshift sync captured local beast action: action={} event={} form={}",
                static_cast<unsigned>(action), GetEventName(action), Multiplayer::GetShapeshiftName(GetLocalForm()));
            if (s_pending.size() < kMaximumQueuedActions)
            {
                s_pending.push_back(action);
            }
        }
    }

    void DiscardLocalActions()
    {
        s_pending.clear();
        const std::scoped_lock lock{ s_capturedLock };
        s_captured.clear();
    }

    void PublishLocalActions(const Network::PlayerTransform& a_checkpoint)
    {
        if (a_checkpoint.furniture.IsEmpty() && a_checkpoint.shapeshift != Shapeshift::kNone)
        {
            const auto graph = GetBeastGraph(a_checkpoint.shapeshift);
            for (const auto action : s_pending)
            {
                if (GetBeastActionGraph(action) == graph)
                {
                    Network::PublishLocalBeastAction(action, a_checkpoint.graphEpoch, a_checkpoint.sequence);
                }
            }
        }
        s_pending.clear();
    }

    void OnCheckpointPlayed(RemoteState& a_state, const CheckpointUpdate& a_update)
    {
        if (a_update.inserted || a_update.previous.graphEpoch == a_update.checkpoint.graphEpoch)
        {
            return;
        }
        const auto newEpoch = a_update.checkpoint.graphEpoch;
        const auto removed = std::erase_if(a_state.pendingActions,
            [newEpoch](const PendingAction& a_pending) { return a_pending.action.graphEpoch != newEpoch; });
        if (removed != 0)
        {
            logs::debug("Shapeshift sync rejected queued beast actions at graph-epoch boundary: player={} "
                        "graphEpoch={} rejected={}",
                a_update.playerID, static_cast<unsigned long long>(newEpoch), removed);
        }
    }

    void AcceptRemoteAction(
        RemoteState* a_state, const Network::PlayerTransform* a_checkpoint, Network::AnimationAction&& a_action)
    {
        if (!a_state || !a_checkpoint || a_action.graphEpoch < a_checkpoint->graphEpoch)
        {
            logs::debug("Shapeshift sync rejected beast action without a compatible checkpoint: player={} "
                        "actionSequence={} graphEpoch={}",
                a_action.playerID, static_cast<unsigned long long>(a_action.actionSequence),
                static_cast<unsigned long long>(a_action.graphEpoch));
            return;
        }
        if (a_state->pendingActions.size() >= kMaximumQueuedActions)
        {
            logs::warn("Shapeshift sync rejected beast action because the bounded queue is full: player={} "
                       "actionSequence={}",
                a_action.playerID, static_cast<unsigned long long>(a_action.actionSequence));
            return;
        }
        a_state->pendingActions.push_back(PendingAction{ .action = std::move(a_action) });
    }

    void ClearRemoteActions(RemoteState& a_state)
    {
        a_state.pendingActions.clear();
    }

    void AgeRemoteActions(const RemoteFrame& a_remote, RemoteState& a_state, float a_delta)
    {
        const float delta = FrameTime::Clamp(a_delta);
        auto& queue = a_state.pendingActions;
        for (auto& pending : queue)
        {
            pending.age += delta;
            if (pending.dispatchAttempts != 0)
            {
                pending.retryElapsed += delta;
            }
        }
        while (!queue.empty() && queue.front().age >= kActionTimeoutSeconds)
        {
            const auto& action = queue.front().action;
            logs::warn("Shapeshift sync timed out queued beast action: player={} action={} event={} "
                       "actionSequence={} form={}",
                a_remote.playerID, static_cast<unsigned>(action.beast), GetEventName(action.beast),
                static_cast<unsigned long long>(action.actionSequence),
                Multiplayer::GetShapeshiftName(a_remote.checkpoint.shapeshift));
            queue.pop_front();
        }
    }

    void SyncBeast(const RemoteFrame& a_remote, RemoteState& a_state, RemoteActor& a_actor, float a_delta)
    {
        PROFILE_FUNCTION();
        const auto& checkpoint = a_remote.checkpoint;
        ActorShapeshift::ReconcileLevitation(a_actor, checkpoint.levitating,
            Multiplayer::IsMoving(checkpoint.animationState.movementState), a_delta);
        ActorShapeshift::ApplyCastRequests(
            a_actor, checkpoint.handPresentation.castingLeft, checkpoint.handPresentation.castingRight);
        ActorShapeshift::ApplyVampireLordPowers(a_actor, checkpoint.mistForm, checkpoint.bats);

        auto& queue = a_state.pendingActions;
        if (queue.empty())
        {
            return;
        }

        // One action per pass, so a burst does not stack edges in one graph update.
        auto& pending = queue.front();
        const auto& action = pending.action;
        if (action.graphEpoch != checkpoint.graphEpoch || action.checkpointSequence > checkpoint.sequence)
        {
            return;
        }
        const auto actionGraph = GetBeastActionGraph(action.beast);
        if (actionGraph != GetBeastGraph(checkpoint.shapeshift))
        {
            logs::debug("Shapeshift sync dropped beast action for another form: player={} action={} event={} "
                        "form={}",
                a_remote.playerID, static_cast<unsigned>(action.beast), GetEventName(action.beast),
                Multiplayer::GetShapeshiftName(checkpoint.shapeshift));
            queue.pop_front();
            return;
        }
        // An actor still in the previous form waits for its respawn.
        if (!a_actor.ready || actionGraph != GetBeastGraph(a_actor.form)
            || (pending.dispatchAttempts != 0 && pending.retryElapsed < kMinimumActionRetrySeconds))
        {
            return;
        }

        const bool accepted = ActorShapeshift::DispatchAction(a_actor, action.beast);
        ++pending.dispatchAttempts;
        pending.retryElapsed = 0.0F;
        if (accepted)
        {
            logs::debug("Shapeshift sync processed beast action: player={} actor={:08X} action={} event={} "
                        "actionSequence={} attempts={} queued={}",
                a_remote.playerID, a_actor.handle.native_handle(), static_cast<unsigned>(action.beast),
                GetEventName(action.beast), static_cast<unsigned long long>(action.actionSequence),
                pending.dispatchAttempts, queue.size() - 1);
            queue.pop_front();
        }
        else if (pending.dispatchAttempts >= kMaximumActionAttempts)
        {
            logs::warn("Shapeshift sync rejected beast action after bounded retries: player={} actor={:08X} "
                       "action={} event={} actionSequence={} attempts={}",
                a_remote.playerID, a_actor.handle.native_handle(), static_cast<unsigned>(action.beast),
                GetEventName(action.beast), static_cast<unsigned long long>(action.actionSequence),
                pending.dispatchAttempts);
            queue.pop_front();
        }
    }
}
