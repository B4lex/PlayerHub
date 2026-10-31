#include "actor/IdlePresentation.h"
#include "actor/ActorAnimation.h"
#include "actor/AnimationFirewall.h"
#include "actor/AttackDispatch.h"
#include "actor/GraphStance.h"
#include "actor/HandPresentation.h"
#include "actor/RemoteActor.h"
#include "actor/WeaponTransition.h"
#include "runtime/FrameTime.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"

#include <memory>
#include <utility>

namespace IdlePresentation
{
    namespace
    {
        constexpr float kIdleSampleSeconds = 0.1F;
        // The idle machine switches state on the graph update after the event, so
        // the default state read right after it does not end the idle.
        constexpr float kIdleEntrySeconds = 0.5F;
    }

    bool CanStart(const RemoteActor& a_remote, const RE::Actor& a_actor, bool a_weaponDrawn)
    {
        const auto& state = a_remote.animation;
        const auto target = ActorAnimation::GetTargetState(a_remote);
        if (!a_remote.ready || !a_remote.animationGraphHolder || !ActorAnimation::IsAnimationStateSettled(a_remote)
            || state.recovery.inProgress || state.dispatch.reassertReason || state.idle.releaseSettling
            || target.jumping || target.weaponDrawn != a_weaponDrawn || AttackDispatch::IsPlaying(a_remote)
            || HandPresentation::IsPlaying(a_remote) || WeaponTransition::IsUnsettled(a_remote))
        {
            return false;
        }
        const auto* actorState = a_actor.AsActorState();
        const auto weaponState = actorState ? actorState->GetWeaponState() : RE::WEAPON_STATE::kWantToDraw;
        if (!actorState || actorState->IsSwimming()
            || weaponState != (a_weaponDrawn ? RE::WEAPON_STATE::kDrawn : RE::WEAPON_STATE::kSheathed))
        {
            return false;
        }
        const auto* holder = static_cast<const RE::IAnimationGraphManagerHolder*>(std::addressof(a_actor));
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphManager;
        return holder == a_remote.animationGraphHolder && a_actor.Is3DLoaded()
            && a_actor.GetAnimationGraphManager(graphManager) && graphManager
            && state.dispatch.graph == graphManager.get();
    }

    bool Dispatch(RemoteActor& a_remote, const RE::BSFixedString& a_event, bool a_exit)
    {
        PROFILE_FUNCTION();
        if (!GameThread::Require("DispatchRemoteIdle"))
        {
            return false;
        }
        const bool accepted = ActorAnimation::SendNetworkAnimationEvent(a_remote, a_event);
        auto& idle = a_remote.animation.idle;
        if (accepted && !a_exit)
        {
            // An exit keeps ownership until its clip has returned the graph to default.
            idle.owned = true;
            idle.observedIdle = false;
            idle.ownedElapsed = 0.0F;
            idle.sampleElapsed = 0.0F;
        }
        return accepted;
    }

    void Release(RemoteActor& a_remote, const char* a_reason)
    {
        auto& idle = a_remote.animation.idle;
        if (!idle.owned || !GameThread::Require("ReleaseRemoteIdle"))
        {
            return;
        }
        auto actor = a_remote.handle.get();
        const bool accepted = actor && AnimationFirewall::ForceDefaultGraphState(*actor);
        logs::debug("Idle sync released remote idle: actor={:08X} reason={} ownedSeconds={:.3f} observedIdle={} "
                    "accepted={}",
            a_remote.handle.native_handle(), a_reason, idle.ownedElapsed, idle.observedIdle, accepted);
        idle.owned = false;
        idle.releaseSettling = accepted;
    }

    void Advance(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime)
    {
        auto& idle = a_remote.animation.idle;
        if (!idle.owned)
        {
            return;
        }
        const float delta = FrameTime::Clamp(a_deltaTime);
        idle.ownedElapsed += delta;
        idle.sampleElapsed += delta;
        if (idle.sampleElapsed < kIdleSampleSeconds)
        {
            return;
        }
        idle.sampleElapsed = 0.0F;
        // A graph without the vanilla idle machines reads kNone, which keeps ownership
        // until the source's state changes.
        const auto reading = GraphStance::ReadStanceSelectors(a_actor).idle;
        if (reading == GraphStance::SelectorState::kOn)
        {
            idle.observedIdle = true;
            return;
        }
        if (reading != GraphStance::SelectorState::kOff || (!idle.observedIdle && idle.ownedElapsed < kIdleEntrySeconds))
        {
            return;
        }
        logs::debug("Idle sync remote idle ended: actor={:08X} ownedSeconds={:.3f} observedIdle={}",
            a_remote.handle.native_handle(), idle.ownedElapsed, idle.observedIdle);
        idle.owned = false;
    }

    bool ConsumeReleaseSettle(RemoteActor& a_remote)
    {
        return std::exchange(a_remote.animation.idle.releaseSettling, false);
    }

    bool IsPlaying(const RemoteActor& a_remote)
    {
        const auto& idle = a_remote.animation.idle;
        return idle.owned || idle.releaseSettling;
    }

    std::uint64_t GetAppliedInstance(const RemoteActor& a_remote)
    {
        return a_remote.animation.idle.appliedInstance;
    }

    void SetAppliedInstance(RemoteActor& a_remote, std::uint64_t a_instance)
    {
        a_remote.animation.idle.appliedInstance = a_instance;
    }
}
