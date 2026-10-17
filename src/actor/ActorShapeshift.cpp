#include "actor/ActorShapeshift.h"
#include "actor/ActorAnimation.h"
#include "actor/BeastActions.h"
#include "actor/GraphStance.h"
#include "actor/RemoteActor.h"
#include "forms/BeastForms.h"
#include "runtime/FrameTime.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"

#include <algorithm>

namespace ActorShapeshift
{
    namespace
    {
        constexpr float kLevitationSampleSeconds = 0.1F;
        // The start and end clips count as the new stance at once, so a mismatch that
        // lasts this long is not a toggle still being taken.
        constexpr float kLevitationMismatchSeconds = 0.3F;
        // Longer than the levitation start and end clips.
        constexpr float kLevitationToggleSpacingSeconds = 1.5F;
        constexpr std::uint32_t kMaximumLevitationToggles = 3;

        // The engine owns and deletes reference effects, so they are found again by
        // target and form rather than kept.
        void StopPowerEffects(RE::Actor& a_actor, const Forms::VampireLordPowerForms& a_forms)
        {
            auto* processLists = RE::ProcessLists::GetSingleton();
            if (!processLists)
            {
                return;
            }
            const auto handle = a_actor.GetHandle().native_handle();
            processLists->ForEachShaderEffect([&](RE::ShaderReferenceEffect* a_effect) {
                if (a_effect && a_effect->target.native_handle() == handle && a_effect->effectData
                    && std::ranges::find(a_forms.shaders, a_effect->effectData) != a_forms.shaders.end())
                {
                    a_effect->finished = true;
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
            processLists->ForEachModelEffect([&](RE::ModelReferenceEffect* a_effect) {
                if (a_effect && a_forms.art && a_effect->target.native_handle() == handle
                    && a_effect->artObject == a_forms.art)
                {
                    a_effect->finished = true;
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }

        // False while the actor cannot show the change yet, so it is tried again.
        bool ShowPower(RemoteActor& a_remote, Forms::VampireLordPower a_power, bool a_shown)
        {
            auto actorPtr = a_remote.handle.get();
            if (!actorPtr || !actorPtr->Is3DLoaded())
            {
                return false;
            }
            auto& actor = *actorPtr;
            const auto& forms = Forms::GetVampireLordPower(a_power);
            if (a_shown)
            {
                for (auto* shader : forms.shaders)
                {
                    if (shader)
                    {
                        (void)actor.ApplyEffectShader(shader, -1.0F);
                    }
                }
                if (forms.art)
                {
                    (void)actor.ApplyArtObject(forms.art, -1.0F);
                }
            }
            else
            {
                StopPowerEffects(actor, forms);
            }
            if (forms.alpha != 1.0F)
            {
                actor.SetAlpha(a_shown ? forms.alpha : 1.0F);
            }
            logs::debug("Shapeshift sync {} vampire lord power: actor={:08X} power={}",
                a_shown ? "showed" : "ended", a_remote.handle.native_handle(),
                a_power == Forms::VampireLordPower::kMistForm ? "mistForm" : "bats");
            return true;
        }
    }

    bool DispatchAction(RemoteActor& a_remote, Multiplayer::BeastAction a_action)
    {
        if (!GameThread::Require("DispatchBeastAction"))
        {
            return false;
        }
        const auto& eventName = BeastActions::GetEventName(a_action);
        auto actorPtr = a_remote.handle.get();
        if (eventName.empty() || !actorPtr)
        {
            return false;
        }
        // As for a human attack or hand presentation: the replica policy's flags can
        // make the graph reject the event, so they open for this synchronous call only.
        auto& actorFlags = actorPtr->GetActorRuntimeData().boolFlags;
        actorFlags.reset(RE::Actor::BOOL_FLAGS::kAttackingDisabled, RE::Actor::BOOL_FLAGS::kCastingDisabled);
        const bool accepted = ActorAnimation::SendNetworkAnimationEvent(a_remote, eventName);
        actorFlags.set(RE::Actor::BOOL_FLAGS::kAttackingDisabled, RE::Actor::BOOL_FLAGS::kCastingDisabled);
        // The attack's graph state rejects locomotion edges until it returns, as for a
        // human attack.
        if (accepted && Multiplayer::IsBeastAttack(a_action))
        {
            ActorAnimation::OwnDispatchedAttack(a_remote, *actorPtr);
        }
        return accepted;
    }

    void ReconcileLevitation(RemoteActor& a_remote, bool a_levitating, bool a_moving, float a_deltaTime)
    {
        PROFILE_FUNCTION();
        auto& state = a_remote.shapeshift;
        const float deltaTime = FrameTime::Clamp(a_deltaTime);
        state.levitationToggleElapsed += deltaTime;
        if (state.levitationTarget != a_levitating)
        {
            state.levitationTarget = a_levitating;
            state.levitationMismatchElapsed = 0.0F;
            state.levitationToggles = 0;
        }
        state.levitationSampleElapsed += deltaTime;
        if (a_remote.form != Multiplayer::Shapeshift::kVampireLord || !a_remote.ready
            || state.levitationSampleElapsed < kLevitationSampleSeconds)
        {
            return;
        }
        const float sampled = state.levitationSampleElapsed;
        state.levitationSampleElapsed = 0.0F;

        auto actorPtr = a_remote.handle.get();
        if (!actorPtr || !actorPtr->Is3DLoaded())
        {
            return;
        }
        const auto reading = GraphStance::ReadStanceSelectors(*actorPtr).levitating;
        const bool known = reading == GraphStance::SelectorState::kOn || reading == GraphStance::SelectorState::kOff;
        if (!known || (reading == GraphStance::SelectorState::kOn) == a_levitating)
        {
            state.levitationMismatchElapsed = 0.0F;
            return;
        }
        state.levitationMismatchElapsed += sampled;
        if (state.levitationMismatchElapsed < kLevitationMismatchSeconds
            || state.levitationToggleElapsed < kLevitationToggleSpacingSeconds
            || state.levitationToggles >= kMaximumLevitationToggles)
        {
            return;
        }

        // The vampire lord's toggle idles (VampireLordLevitateToggle and
        // VampireLordLevitateToggleWhileMoving in Dawnguard.esm) pick the moving variant.
        static const RE::BSFixedString toggleEvent{ "LevitationToggle" };
        static const RE::BSFixedString toggleMovingEvent{ "LevitationToggleMoving" };
        const bool accepted
            = ActorAnimation::SendNetworkAnimationEvent(a_remote, a_moving ? toggleMovingEvent : toggleEvent);
        ++state.levitationToggles;
        state.levitationToggleElapsed = 0.0F;
        state.levitationMismatchElapsed = 0.0F;
        logs::debug("Shapeshift sync toggled vampire lord levitation: actor={:08X} levitating={} moving={} "
                    "accepted={} attempt={}",
            a_remote.handle.native_handle(), a_levitating, a_moving, accepted, state.levitationToggles);
    }

    void ApplyCastRequests(RemoteActor& a_remote, bool a_castingLeft, bool a_castingRight)
    {
        auto& state = a_remote.shapeshift;
        if (a_remote.form != Multiplayer::Shapeshift::kVampireLord || !a_remote.ready
            || (!a_castingLeft && !a_castingRight && !state.castRequested))
        {
            return;
        }
        auto actorPtr = a_remote.handle.get();
        if (!actorPtr)
        {
            return;
        }
        // The cast branch of VampireLord.hkx, like the human magic graph's, stays
        // active only while these are set. A graph reset clears them, so they are
        // written on every pass of a cast.
        static const RE::BSFixedString wantCastLeftVariable{ "bWantCastLeft" };
        static const RE::BSFixedString wantCastRightVariable{ "bWantCastRight" };
        (void)actorPtr->SetGraphVariableBool(wantCastLeftVariable, a_castingLeft);
        (void)actorPtr->SetGraphVariableBool(wantCastRightVariable, a_castingRight);
        state.castRequested = a_castingLeft || a_castingRight;
    }

    void ApplyVampireLordPowers(RemoteActor& a_remote, bool a_mistForm, bool a_bats)
    {
        auto& state = a_remote.shapeshift;
        if (!GameThread::Require("ApplyVampireLordPowers") || !a_remote.ready)
        {
            return;
        }
        // Another form never shows them, so its actor stays as spawned.
        const bool vampireLord = a_remote.form == Multiplayer::Shapeshift::kVampireLord;
        const bool mistForm = vampireLord && a_mistForm;
        const bool bats = vampireLord && a_bats;
        if (state.mistFormShown != mistForm && ShowPower(a_remote, Forms::VampireLordPower::kMistForm, mistForm))
        {
            state.mistFormShown = mistForm;
        }
        if (state.batsShown != bats && ShowPower(a_remote, Forms::VampireLordPower::kBats, bats))
        {
            state.batsShown = bats;
        }
    }
}
