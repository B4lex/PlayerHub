#include "actor/ActorRegistry.h"
#include "actor/AnimationEvents.h"
#include "actor/RemoteActor.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace ActorRegistry
{
    namespace
    {
        using AnimationEvents::IsWeaponTransitionEndEvent;
        using Multiplayer::IsMoving;

        // The main-loop thread reads its own fields without s_mutex (see
        // LockUnlessMainThread) and writes those other threads read under it.
        // Pending counts other threads raise are atomics it consumes without the
        // lock; the observation flags and hook counts are only accessed under it.
        struct Registration
        {
            const RE::Actor* actor{ nullptr };
            const RE::MagicTarget* magicTarget{ nullptr };
            const RE::ActorValueOwner* actorValueOwner{ nullptr };
            std::uint32_t actorHandle{ 0 };
            RE::FormID heldFurnitureRefID{ 0 };
            std::unordered_map<std::string, std::uint32_t> blockedEventCounts;
            std::uint32_t blockedMagicEffects{ 0 };
            std::uint32_t blockedHealthDamage{ 0 };
            std::atomic<std::uint32_t> pendingHitObservations{ 0 };
            std::atomic<std::uint32_t> pendingCombatSanitizations{ 0 };
            std::atomic<std::uint32_t> pendingJumpLandEnds{ 0 };
            std::atomic<std::uint32_t> pendingSwimStarts{ 0 };
            std::atomic<std::uint32_t> pendingSwimStops{ 0 };
            std::atomic<std::uint32_t> pendingWeaponTransitionEnds{ 0 };
            std::atomic<std::uint32_t> pendingBashExits{ 0 };
            std::uint32_t hitObservationCount{ 0 };
            // Main-loop thread only.
            std::uint32_t combatSanitizationCount{ 0 };
            std::uint32_t jumpLandEndCount{ 0 };
            std::uint32_t weaponTransitionEndCount{ 0 };
            std::uint32_t bashExitCount{ 0 };
            bool authoritativeJumping{ false };
            // Network travel direction while moving, in graph Direction units. The
            // actor's direction channel would otherwise feed the remote actor's own (empty)
            // movement into the graph on every animation update.
            float authoritativeDirection{ 0.0F };
            bool authoritativeWeaponDrawn{ false };
            bool animationFirewallEnabled{ false };
            bool furnitureAnimationOwned{ false };
            bool castAnimationOwned{ false };
            bool dead{ false };
            bool mounted{ false };
            // A remote mount or its seated rider, whose riding gait follows the
            // network speed.
            bool gaitDriven{ false };
            float gaitSpeed{ 0.0F };
            // Skyrim's movement update is skipped: the network transform is the only
            // writer of the actor's position and heading.
            bool networkPlaced{ false };
            // The graph Speed of a network-placed actor, which Skyrim no longer
            // measures from its movement.
            float authoritativeSpeed{ 0.0F };
            bool landingCompletionObservationAllowed{ false };
            bool observeJumpLandEndEvent{ false };
            bool observeWeaponTransitionEndEvent{ false };
        };

        PROFILE_LOCKABLE(std::mutex, s_mutex);
        std::unordered_map<const RE::IAnimationGraphManagerHolder*, Registration> s_registrations;
        // Lets the speed channel of every local actor skip the lock while no
        // remote player rides.
        std::atomic<std::size_t> s_gaitDrivenCount{ 0 };
        // Lets the movement update of every local actor skip the lock while no
        // remote actor is network-placed.
        std::atomic<std::size_t> s_networkPlacedCount{ 0 };

        // Called with s_mutex held.
        void EraseRegistration(const RE::IAnimationGraphManagerHolder* a_holder)
        {
            const auto registration = s_registrations.find(a_holder);
            if (registration == s_registrations.end())
            {
                return;
            }
            if (registration->second.gaitDriven)
            {
                s_gaitDrivenCount.fetch_sub(1, std::memory_order_relaxed);
            }
            if (registration->second.networkPlaced)
            {
                s_networkPlacedCount.fetch_sub(1, std::memory_order_relaxed);
            }
            s_registrations.erase(registration);
        }

        // Called with s_mutex held.
        auto FindRegistration(const RE::Actor* a_actor)
        {
            return s_registrations.find(static_cast<const RE::IAnimationGraphManagerHolder*>(a_actor));
        }

        using RegistryLock = std::unique_lock<decltype(s_mutex)>;

        // Registrations are inserted and erased only on the main-loop thread
        // (Register and Unregister run from the spawner and actor runtime), so
        // that thread may find one without s_mutex. Every other thread holds it.
        RegistryLock LockUnlessMainThread()
        {
            return GameThread::IsAuthoritative() ? RegistryLock{} : RegistryLock{ s_mutex };
        }

        // For a write to a field other threads read under s_mutex.
        void LockForWrite(RegistryLock& a_lock)
        {
            if (!a_lock.owns_lock())
            {
                a_lock = RegistryLock{ s_mutex };
            }
        }

        // Called with s_mutex held, or on the main-loop thread.
        Registration* Find(const RE::IAnimationGraphManagerHolder* a_holder)
        {
            const auto registration = s_registrations.find(a_holder);
            return registration != s_registrations.end() ? std::addressof(registration->second) : nullptr;
        }

        Registration* Find(const RE::Actor& a_actor)
        {
            return Find(static_cast<const RE::IAnimationGraphManagerHolder*>(std::addressof(a_actor)));
        }
    }

    void Register(RemoteActor& a_remote, RE::Actor& a_actor)
    {
        auto* holder = static_cast<RE::IAnimationGraphManagerHolder*>(std::addressof(a_actor));
        if (a_remote.animationGraphHolder == holder)
        {
            return;
        }

        std::scoped_lock lock{ s_mutex };
        if (a_remote.animationGraphHolder)
        {
            EraseRegistration(a_remote.animationGraphHolder);
        }
        EraseRegistration(holder);
        // Constructed in place: the atomics make a Registration immovable.
        auto& registration = s_registrations.try_emplace(holder).first->second;
        registration.actor = std::addressof(a_actor);
        registration.magicTarget = a_actor.AsMagicTarget();
        registration.actorValueOwner = a_actor.AsActorValueOwner();
        registration.actorHandle = a_remote.handle.native_handle();
        registration.authoritativeJumping = a_remote.animation->targetAnimationState.jumping;
        registration.authoritativeDirection = IsMoving(a_remote.animation->targetAnimationState.movementState)
            ? a_remote.movement.animationDirection
            : 0.0F;
        registration.authoritativeWeaponDrawn = a_remote.animation->targetAnimationState.weaponDrawn;
        registration.furnitureAnimationOwned = a_remote.furniture.furnitureAnimationOwned;
        registration.networkPlaced = a_remote.collision.networkPlaced;
        registration.authoritativeSpeed = a_remote.animation->publishedGraphSpeed;
        registration.observeWeaponTransitionEndEvent = a_remote.animation->targetAnimationState.weaponDrawn;
        if (a_remote.collision.networkPlaced)
        {
            s_networkPlacedCount.fetch_add(1, std::memory_order_relaxed);
        }
        a_remote.animationGraphHolder = holder;
    }

    void Unregister(RemoteActor& a_remote)
    {
        if (!a_remote.animationGraphHolder)
        {
            return;
        }

        std::scoped_lock lock{ s_mutex };
        EraseRegistration(a_remote.animationGraphHolder);
        a_remote.animationGraphHolder = nullptr;
    }

    // Safe to call from AI worker threads.
    bool IsRegistered(const RE::Actor* a_actor)
    {
        if (!a_actor)
        {
            return false;
        }

        std::scoped_lock lock{ s_mutex };
        return s_registrations.contains(static_cast<const RE::IAnimationGraphManagerHolder*>(a_actor));
    }

    bool IsRegistered(RE::ActorHandle a_handle)
    {
        const auto nativeHandle = a_handle.native_handle();
        if (nativeHandle == 0)
        {
            return false;
        }

        std::scoped_lock lock{ s_mutex };
        return std::ranges::any_of(s_registrations,
            [nativeHandle](const auto& a_entry) { return a_entry.second.actorHandle == nativeHandle; });
    }

    bool EnableAnimationFirewall(const RemoteActor& a_remote)
    {
        auto lock = LockUnlessMainThread();
        auto* registration = Find(a_remote.animationGraphHolder);
        if (!registration || registration->animationFirewallEnabled)
        {
            return false;
        }
        LockForWrite(lock);
        registration->animationFirewallEnabled = true;
        return true;
    }

    void DisableAnimationFirewall(const RemoteActor& a_remote)
    {
        auto lock = LockUnlessMainThread();
        auto* registration = Find(a_remote.animationGraphHolder);
        if (registration && registration->animationFirewallEnabled)
        {
            LockForWrite(lock);
            registration->animationFirewallEnabled = false;
        }
    }

    void SetFurnitureAnimationOwned(const RemoteActor& a_remote, bool a_owned)
    {
        std::scoped_lock lock{ s_mutex };
        const auto registration = s_registrations.find(a_remote.animationGraphHolder);
        if (registration != s_registrations.end())
        {
            registration->second.furnitureAnimationOwned = a_owned;
        }
    }

    void SetHeldFurniture(const RemoteActor& a_remote, RE::FormID a_furnitureRefID)
    {
        auto lock = LockUnlessMainThread();
        auto* registration = Find(a_remote.animationGraphHolder);
        if (registration && registration->heldFurnitureRefID != a_furnitureRefID)
        {
            LockForWrite(lock);
            registration->heldFurnitureRefID = a_furnitureRefID;
        }
    }

    bool IsFurnitureHeld(RE::FormID a_furnitureRefID)
    {
        if (a_furnitureRefID == 0)
        {
            return false;
        }

        std::scoped_lock lock{ s_mutex };
        return std::ranges::any_of(s_registrations,
            [a_furnitureRefID](const auto& a_entry) { return a_entry.second.heldFurnitureRefID == a_furnitureRefID; });
    }

    void SetCastAnimationOwned(const RemoteActor& a_remote, bool a_owned)
    {
        std::scoped_lock lock{ s_mutex };
        const auto registration = s_registrations.find(a_remote.animationGraphHolder);
        if (registration != s_registrations.end())
        {
            registration->second.castAnimationOwned = a_owned;
        }
    }

    bool IsCastAnimationOwned(const RE::Actor* a_actor)
    {
        if (!a_actor)
        {
            return false;
        }

        const auto lock = LockUnlessMainThread();
        const auto* registration = Find(*a_actor);
        return registration && registration->castAnimationOwned;
    }

    void SetDead(const RemoteActor& a_remote)
    {
        std::scoped_lock lock{ s_mutex };
        const auto registration = s_registrations.find(a_remote.animationGraphHolder);
        if (registration != s_registrations.end())
        {
            registration->second.dead = true;
        }
    }

    void SetMounted(const RemoteActor& a_remote, bool a_mounted)
    {
        std::scoped_lock lock{ s_mutex };
        const auto registration = s_registrations.find(a_remote.animationGraphHolder);
        if (registration == s_registrations.end())
        {
            return;
        }
        registration->second.mounted = a_mounted;
        if (!a_mounted && registration->second.gaitDriven)
        {
            registration->second.gaitDriven = false;
            s_gaitDrivenCount.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    void SetGaitSpeed(const RemoteActor& a_remote, float a_speed)
    {
        std::scoped_lock lock{ s_mutex };
        const auto registration = s_registrations.find(a_remote.animationGraphHolder);
        if (registration == s_registrations.end())
        {
            return;
        }
        if (!registration->second.gaitDriven)
        {
            registration->second.gaitDriven = true;
            s_gaitDrivenCount.fetch_add(1, std::memory_order_relaxed);
        }
        registration->second.gaitSpeed = a_speed;
    }

    void SetNetworkPlaced(const RemoteActor& a_remote, bool a_networkPlaced)
    {
        std::scoped_lock lock{ s_mutex };
        const auto registration = s_registrations.find(a_remote.animationGraphHolder);
        if (registration == s_registrations.end() || registration->second.networkPlaced == a_networkPlaced)
        {
            return;
        }
        registration->second.networkPlaced = a_networkPlaced;
        if (a_networkPlaced)
        {
            s_networkPlacedCount.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            s_networkPlacedCount.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    bool IsNetworkPlaced(const RE::Actor* a_actor)
    {
        if (!a_actor || s_networkPlacedCount.load(std::memory_order_relaxed) == 0)
        {
            return false;
        }

        std::scoped_lock lock{ s_mutex };
        const auto registration = s_registrations.find(static_cast<const RE::IAnimationGraphManagerHolder*>(a_actor));
        // A dead actor is left to Skyrim's death and ragdoll.
        return registration != s_registrations.end() && registration->second.networkPlaced
            && !registration->second.dead;
    }

    void SetAuthoritativeSpeed(const RemoteActor& a_remote, float a_speed)
    {
        std::scoped_lock lock{ s_mutex };
        const auto registration = s_registrations.find(a_remote.animationGraphHolder);
        if (registration != s_registrations.end())
        {
            registration->second.authoritativeSpeed = a_speed;
        }
    }

    std::optional<float> GetAuthoritativeSpeed(const RE::Actor* a_actor)
    {
        if (!a_actor
            || (s_gaitDrivenCount.load(std::memory_order_relaxed) == 0
                && s_networkPlacedCount.load(std::memory_order_relaxed) == 0))
        {
            return std::nullopt;
        }

        std::scoped_lock lock{ s_mutex };
        const auto registration = s_registrations.find(static_cast<const RE::IAnimationGraphManagerHolder*>(a_actor));
        if (registration == s_registrations.end())
        {
            return std::nullopt;
        }
        if (registration->second.gaitDriven)
        {
            return registration->second.gaitSpeed;
        }
        return registration->second.networkPlaced && !registration->second.dead
            ? std::optional<float>{ registration->second.authoritativeSpeed }
            : std::nullopt;
    }

    std::optional<GraphInput> FindGraphInput(const RE::IAnimationGraphManagerHolder* a_holder)
    {
        std::scoped_lock lock{ s_mutex };
        const auto registration = s_registrations.find(a_holder);
        if (registration == s_registrations.end())
        {
            return std::nullopt;
        }
        return GraphInput{ .actorHandle = registration->second.actorHandle,
            .animationFirewallEnabled = registration->second.animationFirewallEnabled,
            .furnitureAnimationOwned = registration->second.furnitureAnimationOwned,
            .castAnimationOwned = registration->second.castAnimationOwned,
            .dead = registration->second.dead,
            .mounted = registration->second.mounted,
            .gaitDriven = registration->second.gaitDriven };
    }

    void RecordSwimTransition(const RE::IAnimationGraphManagerHolder* a_holder, bool a_started)
    {
        std::scoped_lock lock{ s_mutex };
        const auto registration = s_registrations.find(a_holder);
        if (registration != s_registrations.end())
        {
            ++(a_started ? registration->second.pendingSwimStarts : registration->second.pendingSwimStops);
        }
    }

    std::uint32_t RecordBlockedGraphEvent(const RE::IAnimationGraphManagerHolder* a_holder, const char* a_eventName)
    {
        std::scoped_lock lock{ s_mutex };
        const auto registration = s_registrations.find(a_holder);
        if (registration == s_registrations.end())
        {
            return 0;
        }

        auto& count = registration->second.blockedEventCounts[a_eventName ? a_eventName : "<null>"];
        const auto blockedEventCount = ++count;
        if (a_eventName && std::string_view{ a_eventName } == "IdleCowering")
        {
            ++registration->second.pendingCombatSanitizations;
        }
        return blockedEventCount;
    }

    GraphOutputObservation ObserveGraphOutputEvent(const RE::TESObjectREFR* a_holder, std::string_view a_eventName,
        bool a_isJumpLandEnd, bool a_shouldTraceWeaponEvent)
    {
        GraphOutputObservation observation;
        std::scoped_lock lock{ s_mutex };
        const auto registration = s_registrations.find(static_cast<const RE::IAnimationGraphManagerHolder*>(a_holder));
        if (registration != s_registrations.end() && a_isJumpLandEnd
            && registration->second.landingCompletionObservationAllowed && registration->second.observeJumpLandEndEvent)
        {
            ++registration->second.pendingJumpLandEnds;
            observation.landingCompletionCount = ++registration->second.jumpLandEndCount;
            registration->second.observeJumpLandEndEvent = false;
            observation.actorHandle = registration->second.actorHandle;
        }
        if (registration != s_registrations.end() && registration->second.observeWeaponTransitionEndEvent
            && a_shouldTraceWeaponEvent)
        {
            observation.traceWeaponEvent = true;
            observation.expectedWeaponDrawn = registration->second.authoritativeWeaponDrawn;
            observation.actorHandle = registration->second.actorHandle;
        }
        if (registration != s_registrations.end() && registration->second.observeWeaponTransitionEndEvent
            && IsWeaponTransitionEndEvent(a_eventName, registration->second.authoritativeWeaponDrawn))
        {
            ++registration->second.pendingWeaponTransitionEnds;
            observation.weaponCompletionCount = ++registration->second.weaponTransitionEndCount;
            registration->second.observeWeaponTransitionEndEvent = false;
            observation.actorHandle = registration->second.actorHandle;
        }
        if (registration != s_registrations.end() && AnimationEvents::EqualsIgnoringCase(a_eventName, "bashExit"))
        {
            ++registration->second.pendingBashExits;
            observation.bashExitCount = ++registration->second.bashExitCount;
            observation.actorHandle = registration->second.actorHandle;
        }
        return observation;
    }

    std::optional<UpdatePolicy> GetUpdatePolicy(const RE::Actor* a_actor)
    {
        if (!a_actor)
        {
            return std::nullopt;
        }

        std::scoped_lock lock{ s_mutex };
        const auto registration = FindRegistration(a_actor);
        if (registration == s_registrations.end())
        {
            return std::nullopt;
        }

        return UpdatePolicy{ .authoritativeJumping = registration->second.authoritativeJumping,
            .dead = registration->second.dead };
    }

    void SetAuthoritativeJumpState(const RemoteActor& a_remote, bool a_jumping)
    {
        auto lock = LockUnlessMainThread();
        auto* registration = Find(a_remote.animationGraphHolder);
        if (registration && registration->authoritativeJumping != a_jumping)
        {
            LockForWrite(lock);
            registration->authoritativeJumping = a_jumping;
        }
    }

    void SetAuthoritativeDirection(const RemoteActor& a_remote)
    {
        const float direction = IsMoving(a_remote.animation->targetAnimationState.movementState)
            ? a_remote.movement.animationDirection
            : 0.0F;
        auto lock = LockUnlessMainThread();
        auto* registration = Find(a_remote.animationGraphHolder);
        if (registration && registration->authoritativeDirection != direction)
        {
            LockForWrite(lock);
            registration->authoritativeDirection = direction;
        }
    }

    std::optional<float> GetAuthoritativeDirection(const RE::Actor* a_actor)
    {
        if (!a_actor)
        {
            return std::nullopt;
        }

        std::scoped_lock lock{ s_mutex };
        const auto registration = FindRegistration(a_actor);
        if (registration == s_registrations.end())
        {
            return std::nullopt;
        }
        // A riding gait only travels the way the mount faces. A rider's on-foot
        // direction is stale once it is seated.
        return registration->second.gaitDriven ? 0.0F : registration->second.authoritativeDirection;
    }

    void SetAuthoritativeWeaponDrawnState(const RemoteActor& a_remote, bool a_weaponDrawn)
    {
        auto lock = LockUnlessMainThread();
        auto* registration = Find(a_remote.animationGraphHolder);
        if (!registration || registration->authoritativeWeaponDrawn == a_weaponDrawn)
        {
            return;
        }
        // The worker observing the completion reads the state, the flag, and the
        // count together.
        LockForWrite(lock);
        registration->authoritativeWeaponDrawn = a_weaponDrawn;
        // Arm before DrawWeaponMagicHands runs on the next remote actor update. Its
        // completion annotation may be emitted from an animation worker.
        registration->pendingWeaponTransitionEnds = 0;
        registration->observeWeaponTransitionEndEvent = true;
    }

    std::optional<bool> GetAuthoritativeWeaponDrawnState(const RE::Actor& a_actor)
    {
        const auto lock = LockUnlessMainThread();
        const auto* registration = Find(a_actor);
        return registration ? std::optional<bool>{ registration->authoritativeWeaponDrawn } : std::nullopt;
    }

    std::pair<std::uint32_t, std::uint32_t> BlockMagicEffect(const RE::MagicTarget* a_target)
    {
        std::scoped_lock lock{ s_mutex };
        const auto registration = std::ranges::find_if(
            s_registrations, [a_target](const auto& a_entry) { return a_entry.second.magicTarget == a_target; });
        if (registration == s_registrations.end())
        {
            return {};
        }

        return { registration->second.actorHandle, ++registration->second.blockedMagicEffects };
    }

    std::pair<std::uint32_t, std::uint32_t> BlockHealthDamage(const RE::Actor* a_actor)
    {
        if (!a_actor)
        {
            return {};
        }

        std::scoped_lock lock{ s_mutex };
        const auto registration = FindRegistration(a_actor);
        if (registration == s_registrations.end() || registration->second.dead)
        {
            return {};
        }

        return { registration->second.actorHandle, ++registration->second.blockedHealthDamage };
    }

    std::pair<std::uint32_t, std::uint32_t> BlockHealthActorValueDamage(const RE::ActorValueOwner* a_actorValueOwner)
    {
        if (!a_actorValueOwner)
        {
            return {};
        }

        std::scoped_lock lock{ s_mutex };
        const auto registration = std::ranges::find_if(s_registrations,
            [a_actorValueOwner](const auto& a_entry) { return a_entry.second.actorValueOwner == a_actorValueOwner; });
        if (registration == s_registrations.end() || registration->second.dead)
        {
            return {};
        }

        return { registration->second.actorHandle, ++registration->second.blockedHealthDamage };
    }

    std::pair<std::uint32_t, std::uint32_t> RequestHitObservation(const RE::Actor* a_actor)
    {
        if (!a_actor)
        {
            return {};
        }

        std::scoped_lock lock{ s_mutex };
        const auto registration = FindRegistration(a_actor);
        if (registration == s_registrations.end())
        {
            return {};
        }

        ++registration->second.pendingHitObservations;
        return { registration->second.actorHandle, ++registration->second.hitObservationCount };
    }

    std::uint32_t ConsumeHitObservations(const RemoteActor& a_remote)
    {
        const auto lock = LockUnlessMainThread();
        auto* registration = Find(a_remote.animationGraphHolder);
        return registration ? registration->pendingHitObservations.exchange(0) : 0;
    }

    std::uint32_t ConsumeSwimStarts(const RemoteActor& a_remote)
    {
        const auto lock = LockUnlessMainThread();
        auto* registration = Find(a_remote.animationGraphHolder);
        return registration ? registration->pendingSwimStarts.exchange(0) : 0;
    }

    std::uint32_t ConsumeSwimStops(const RemoteActor& a_remote)
    {
        const auto lock = LockUnlessMainThread();
        auto* registration = Find(a_remote.animationGraphHolder);
        return registration ? registration->pendingSwimStops.exchange(0) : 0;
    }

    std::uint32_t ConsumeWeaponTransitionEnds(const RemoteActor& a_remote)
    {
        const auto lock = LockUnlessMainThread();
        auto* registration = Find(a_remote.animationGraphHolder);
        return registration ? registration->pendingWeaponTransitionEnds.exchange(0) : 0;
    }

    void SetLandingCompletionObservation(const RE::Actor& a_actor, bool a_allowed)
    {
        auto lock = LockUnlessMainThread();
        auto* registration = Find(a_actor);
        // While disallowed nothing arms the observation or raises its count, so
        // disallowing it again changes nothing.
        if (!registration || (!a_allowed && !registration->landingCompletionObservationAllowed))
        {
            return;
        }
        LockForWrite(lock);
        registration->landingCompletionObservationAllowed = a_allowed;
        registration->observeJumpLandEndEvent = false;
        registration->pendingJumpLandEnds = 0;
    }

    void ArmJumpLandEndObservation(const RE::Actor& a_actor)
    {
        auto lock = LockUnlessMainThread();
        auto* registration = Find(a_actor);
        if (registration && registration->landingCompletionObservationAllowed)
        {
            LockForWrite(lock);
            // Arm only after the grounded transform and graph variables are in
            // place. A delayed marker from the prior jump must not complete this one.
            registration->pendingJumpLandEnds = 0;
            registration->observeJumpLandEndEvent = true;
        }
    }

    std::uint32_t ConsumeJumpLandEnds(const RemoteActor& a_remote)
    {
        const auto lock = LockUnlessMainThread();
        auto* registration = Find(a_remote.animationGraphHolder);
        return registration ? registration->pendingJumpLandEnds.exchange(0) : 0;
    }

    std::uint32_t ConsumeBashExits(const RE::Actor& a_actor)
    {
        const auto lock = LockUnlessMainThread();
        auto* registration = Find(a_actor);
        return registration ? registration->pendingBashExits.exchange(0) : 0;
    }

    std::uint32_t ConsumeCombatSanitizationRequests(const RE::Actor& a_actor)
    {
        const auto lock = LockUnlessMainThread();
        auto* registration = Find(a_actor);
        return registration ? registration->pendingCombatSanitizations.exchange(0) : 0;
    }

    std::pair<std::uint32_t, std::uint32_t> RecordCombatSanitization(const RE::Actor& a_actor)
    {
        if (!GameThread::IsAuthoritative())
        {
            return {};
        }

        auto* registration = Find(a_actor);
        return registration ? std::pair{ registration->actorHandle, ++registration->combatSanitizationCount }
                            : std::pair<std::uint32_t, std::uint32_t>{};
    }
}
