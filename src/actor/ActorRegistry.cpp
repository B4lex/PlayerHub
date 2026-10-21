#include "actor/ActorRegistry.h"
#include "actor/AnimationEvents.h"
#include "actor/RemoteActor.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"
#include "text/AsciiCase.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace ActorRegistry
{
    namespace
    {
        using AnimationEvents::IsWeaponTransitionEndEvent;

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
            std::atomic<std::uint32_t> pendingFurnitureExitClipEnds{ 0 };
            std::uint32_t hitObservationCount{ 0 };
            // Main-loop thread only.
            std::uint32_t combatSanitizationCount{ 0 };
            std::uint32_t jumpLandEndCount{ 0 };
            std::uint32_t weaponTransitionEndCount{ 0 };
            std::uint32_t bashExitCount{ 0 };
            bool authoritativeJumping{ false };
            float authoritativeDirection{ 0.0F };
            bool authoritativeWeaponDrawn{ false };
            bool animationFirewallEnabled{ false };
            bool furnitureAnimationOwned{ false };
            bool nativeFurnitureExit{ false };
            bool castAnimationOwned{ false };
            bool dead{ false };
            bool mounted{ false };
            bool gaitDriven{ false };
            float gaitSpeed{ 0.0F };
            bool networkPlaced{ false };
            float authoritativeSpeed{ 0.0F };
            float attackSpeedScale{ 1.0F };
            bool landingCompletionObservationAllowed{ false };
            bool observeJumpLandEndEvent{ false };
            bool observeWeaponTransitionEndEvent{ false };
        };

        PROFILE_SHARED_LOCKABLE(std::shared_mutex, s_mutex);
        std::unordered_map<const RE::IAnimationGraphManagerHolder*, Registration> s_registrations;
        // Point into s_registrations, whose elements keep their address until erased.
        std::unordered_map<const RE::MagicTarget*, Registration*> s_byMagicTarget;
        std::unordered_map<const RE::ActorValueOwner*, Registration*> s_byActorValueOwner;
        std::unordered_map<std::uint32_t, Registration*> s_byHandle;
        std::atomic<std::size_t> s_gaitDrivenCount{ 0 };
        std::atomic<std::size_t> s_networkPlacedCount{ 0 };
        // Registrations whose attackSpeedScale is not 1.
        std::atomic<std::size_t> s_attackSpeedScaledCount{ 0 };
        std::atomic<std::size_t> s_nativeFurnitureExitCount{ 0 };

        template <class Key>
        void IndexRegistration(std::unordered_map<Key, Registration*>& a_index, Key a_key, Registration& a_registration)
        {
            if (a_key)
            {
                a_index.insert_or_assign(a_key, std::addressof(a_registration));
            }
        }

        template <class Key>
        void UnindexRegistration(
            std::unordered_map<Key, Registration*>& a_index, Key a_key, const Registration& a_registration)
        {
            const auto entry = a_index.find(a_key);
            if (entry != a_index.end() && entry->second == std::addressof(a_registration))
            {
                a_index.erase(entry);
            }
        }

        template <class Key>
        Registration* FindIndexed(const std::unordered_map<Key, Registration*>& a_index, Key a_key)
        {
            const auto entry = a_index.find(a_key);
            return entry != a_index.end() ? entry->second : nullptr;
        }

        void EraseRegistration(const RE::IAnimationGraphManagerHolder* a_holder)
        {
            const auto registration = s_registrations.find(a_holder);
            if (registration == s_registrations.end())
            {
                return;
            }
            UnindexRegistration(s_byMagicTarget, registration->second.magicTarget, registration->second);
            UnindexRegistration(s_byActorValueOwner, registration->second.actorValueOwner, registration->second);
            UnindexRegistration(s_byHandle, registration->second.actorHandle, registration->second);
            if (registration->second.nativeFurnitureExit)
            {
                s_nativeFurnitureExitCount.fetch_sub(1, std::memory_order_relaxed);
            }
            if (registration->second.gaitDriven)
            {
                s_gaitDrivenCount.fetch_sub(1, std::memory_order_relaxed);
            }
            if (registration->second.networkPlaced)
            {
                s_networkPlacedCount.fetch_sub(1, std::memory_order_relaxed);
            }
            if (registration->second.attackSpeedScale != 1.0F)
            {
                s_attackSpeedScaledCount.fetch_sub(1, std::memory_order_relaxed);
            }
            s_registrations.erase(registration);
        }

        using RegistryLock = std::unique_lock<decltype(s_mutex)>;
        using SharedRegistryLock = std::shared_lock<decltype(s_mutex)>;

        RegistryLock LockUnlessMainThread()
        {
            return GameThread::IsAuthoritative() ? RegistryLock{} : RegistryLock{ s_mutex };
        }

        // For reading fields only the main-loop thread writes, or raising an atomic count.
        SharedRegistryLock LockSharedUnlessMainThread()
        {
            return GameThread::IsAuthoritative() ? SharedRegistryLock{} : SharedRegistryLock{ s_mutex };
        }

        void LockForWrite(RegistryLock& a_lock)
        {
            if (!a_lock.owns_lock())
            {
                a_lock = RegistryLock{ s_mutex };
            }
        }

        Registration* Find(const RE::IAnimationGraphManagerHolder* a_holder)
        {
            const auto registration = s_registrations.find(a_holder);
            return registration != s_registrations.end() ? std::addressof(registration->second) : nullptr;
        }

        // Sets a field that other threads read, locking only to change it.
        template <class T>
        void SetShared(const RemoteActor& a_remote, T Registration::* a_field, T a_value)
        {
            auto lock = LockUnlessMainThread();
            auto* registration = Find(a_remote.animationGraphHolder);
            if (registration && registration->*a_field != a_value)
            {
                LockForWrite(lock);
                registration->*a_field = a_value;
            }
        }

        // A flag whose set registrations a hook counts to skip the lock when none is set.
        void SetCounted(
            const RemoteActor& a_remote, bool Registration::* a_flag, std::atomic<std::size_t>& a_count, bool a_value)
        {
            auto lock = LockUnlessMainThread();
            auto* registration = Find(a_remote.animationGraphHolder);
            if (!registration || registration->*a_flag == a_value)
            {
                return;
            }
            LockForWrite(lock);
            registration->*a_flag = a_value;
            if (a_value)
            {
                a_count.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                a_count.fetch_sub(1, std::memory_order_relaxed);
            }
        }

        Registration* Find(const RE::Actor* a_actor)
        {
            return a_actor ? Find(static_cast<const RE::IAnimationGraphManagerHolder*>(a_actor)) : nullptr;
        }

        Registration* Find(const RE::Actor& a_actor)
        {
            return Find(std::addressof(a_actor));
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
        registration.authoritativeJumping = a_remote.animation.targetAnimationState.jumping;
        registration.authoritativeDirection = a_remote.movement.animationDirection;
        registration.authoritativeWeaponDrawn = a_remote.animation.targetAnimationState.weaponDrawn;
        registration.furnitureAnimationOwned = a_remote.furniture.furnitureAnimationOwned;
        registration.networkPlaced = a_remote.collision.networkPlaced;
        registration.authoritativeSpeed = a_remote.animation.publishedGraphSpeed;
        registration.observeWeaponTransitionEndEvent = a_remote.animation.targetAnimationState.weaponDrawn;
        IndexRegistration(s_byMagicTarget, registration.magicTarget, registration);
        IndexRegistration(s_byActorValueOwner, registration.actorValueOwner, registration);
        IndexRegistration(s_byHandle, registration.actorHandle, registration);
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

    bool IsRegistered(const RE::Actor* a_actor)
    {
        if (!a_actor)
        {
            return false;
        }

        const auto lock = LockSharedUnlessMainThread();
        return Find(a_actor) != nullptr;
    }

    bool IsRegistered(RE::ActorHandle a_handle)
    {
        const auto nativeHandle = a_handle.native_handle();
        if (nativeHandle == 0)
        {
            return false;
        }

        const auto lock = LockSharedUnlessMainThread();
        return FindIndexed(s_byHandle, nativeHandle) != nullptr;
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
        SetShared(a_remote, &Registration::furnitureAnimationOwned, a_owned);
    }

    void SetHeldFurniture(const RemoteActor& a_remote, RE::FormID a_furnitureRefID)
    {
        SetShared(a_remote, &Registration::heldFurnitureRefID, a_furnitureRefID);
    }

    bool IsFurnitureHeld(RE::FormID a_furnitureRefID)
    {
        if (a_furnitureRefID == 0)
        {
            return false;
        }

        const auto lock = LockSharedUnlessMainThread();
        return std::ranges::any_of(s_registrations,
            [a_furnitureRefID](const auto& a_entry) { return a_entry.second.heldFurnitureRefID == a_furnitureRefID; });
    }

    void SetNativeFurnitureExit(const RemoteActor& a_remote, bool a_exiting)
    {
        SetCounted(a_remote, &Registration::nativeFurnitureExit, s_nativeFurnitureExitCount, a_exiting);
    }

    void ObserveFurnitureExitGraphOutput(const RE::TESObjectREFR* a_holder, std::string_view a_eventName)
    {
        if (!a_holder || s_nativeFurnitureExitCount.load(std::memory_order_relaxed) == 0
            || !AsciiCase::Equals(a_eventName, "IdleFurnitureExit"))
        {
            return;
        }

        const auto lock = LockSharedUnlessMainThread();
        auto* registration = Find(static_cast<const RE::IAnimationGraphManagerHolder*>(a_holder));
        if (registration && registration->nativeFurnitureExit)
        {
            ++registration->pendingFurnitureExitClipEnds;
        }
    }

    std::uint32_t ConsumeFurnitureExitClipEnds(const RemoteActor& a_remote)
    {
        const auto lock = LockUnlessMainThread();
        auto* registration = Find(a_remote.animationGraphHolder);
        return registration ? registration->pendingFurnitureExitClipEnds.exchange(0) : 0;
    }

    void SetCastAnimationOwned(const RemoteActor& a_remote, bool a_owned)
    {
        SetShared(a_remote, &Registration::castAnimationOwned, a_owned);
    }

    bool IsCastAnimationOwned(const RE::Actor* a_actor)
    {
        if (!a_actor)
        {
            return false;
        }

        const auto lock = LockSharedUnlessMainThread();
        const auto* registration = Find(a_actor);
        return registration && registration->castAnimationOwned;
    }

    void SetDead(const RemoteActor& a_remote)
    {
        SetShared(a_remote, &Registration::dead, true);
    }

    void SetMounted(const RemoteActor& a_remote, bool a_mounted)
    {
        SetShared(a_remote, &Registration::mounted, a_mounted);
    }

    void SetGaitSpeed(const RemoteActor& a_remote, float a_speed)
    {
        SetShared(a_remote, &Registration::gaitSpeed, a_speed);
        SetCounted(a_remote, &Registration::gaitDriven, s_gaitDrivenCount, true);
    }

    void SetNetworkPlaced(const RemoteActor& a_remote, bool a_networkPlaced)
    {
        SetCounted(a_remote, &Registration::networkPlaced, s_networkPlacedCount, a_networkPlaced);
    }

    bool IsNetworkPlaced(const RE::Actor* a_actor)
    {
        if (!a_actor || s_networkPlacedCount.load(std::memory_order_relaxed) == 0)
        {
            return false;
        }

        const auto lock = LockSharedUnlessMainThread();
        const auto* registration = Find(a_actor);
        // A dead actor is left to Skyrim's death and ragdoll.
        return registration && registration->networkPlaced && !registration->dead;
    }

    void SetAttackSpeedScale(const RemoteActor& a_remote, float a_scale)
    {
        auto lock = LockUnlessMainThread();
        auto* registration = Find(a_remote.animationGraphHolder);
        if (!registration || registration->attackSpeedScale == a_scale)
        {
            return;
        }
        LockForWrite(lock);
        const bool wasScaled = registration->attackSpeedScale != 1.0F;
        const bool scaled = a_scale != 1.0F;
        registration->attackSpeedScale = a_scale;
        if (scaled && !wasScaled)
        {
            s_attackSpeedScaledCount.fetch_add(1, std::memory_order_relaxed);
        }
        else if (!scaled && wasScaled)
        {
            s_attackSpeedScaledCount.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    std::optional<float> GetAttackSpeedScale(const RE::Actor* a_actor)
    {
        if (!a_actor || s_attackSpeedScaledCount.load(std::memory_order_relaxed) == 0)
        {
            return std::nullopt;
        }

        const auto lock = LockSharedUnlessMainThread();
        const auto* registration = Find(a_actor);
        if (!registration || registration->attackSpeedScale == 1.0F)
        {
            return std::nullopt;
        }
        return registration->attackSpeedScale;
    }

    void SetAuthoritativeSpeed(const RemoteActor& a_remote, float a_speed)
    {
        SetShared(a_remote, &Registration::authoritativeSpeed, a_speed);
    }

    std::optional<float> GetGaitSpeed(const RE::Actor* a_actor)
    {
        if (!a_actor || s_gaitDrivenCount.load(std::memory_order_relaxed) == 0)
        {
            return std::nullopt;
        }

        const auto lock = LockSharedUnlessMainThread();
        const auto* registration = Find(a_actor);
        return registration && registration->gaitDriven ? std::optional<float>{ registration->gaitSpeed }
                                                        : std::nullopt;
    }

    std::optional<float> GetAuthoritativeSpeed(const RE::Actor* a_actor)
    {
        if (!a_actor
            || (s_gaitDrivenCount.load(std::memory_order_relaxed) == 0
                && s_networkPlacedCount.load(std::memory_order_relaxed) == 0))
        {
            return std::nullopt;
        }

        const auto lock = LockSharedUnlessMainThread();
        const auto* registration = Find(a_actor);
        if (!registration)
        {
            return std::nullopt;
        }
        if (registration->gaitDriven)
        {
            return registration->gaitSpeed;
        }
        return registration->networkPlaced && !registration->dead
            ? std::optional<float>{ registration->authoritativeSpeed }
            : std::nullopt;
    }

    std::optional<GraphInput> FindGraphInput(const RE::IAnimationGraphManagerHolder* a_holder)
    {
        const auto lock = LockSharedUnlessMainThread();
        const auto* registration = Find(a_holder);
        if (!registration)
        {
            return std::nullopt;
        }
        return GraphInput{ .actorHandle = registration->actorHandle,
            .animationFirewallEnabled = registration->animationFirewallEnabled,
            .furnitureAnimationOwned = registration->furnitureAnimationOwned,
            .castAnimationOwned = registration->castAnimationOwned,
            .dead = registration->dead,
            .mounted = registration->mounted,
            .gaitDriven = registration->gaitDriven };
    }

    void RecordSwimTransition(const RE::IAnimationGraphManagerHolder* a_holder, bool a_started)
    {
        const auto lock = LockSharedUnlessMainThread();
        if (auto* registration = Find(a_holder))
        {
            ++(a_started ? registration->pendingSwimStarts : registration->pendingSwimStops);
        }
    }

    std::uint32_t RecordBlockedGraphEvent(const RE::IAnimationGraphManagerHolder* a_holder, const char* a_eventName)
    {
        std::scoped_lock lock{ s_mutex };
        auto* registration = Find(a_holder);
        if (!registration)
        {
            return 0;
        }

        auto& count = registration->blockedEventCounts[a_eventName ? a_eventName : "<null>"];
        const auto blockedEventCount = ++count;
        if (a_eventName && std::string_view{ a_eventName } == "IdleCowering")
        {
            ++registration->pendingCombatSanitizations;
        }
        return blockedEventCount;
    }

    GraphOutputObservation ObserveGraphOutputEvent(const RE::TESObjectREFR* a_holder, std::string_view a_eventName,
        bool a_isJumpLandEnd, bool a_shouldTraceWeaponEvent)
    {
        GraphOutputObservation observation;
        std::scoped_lock lock{ s_mutex };
        auto* registration = Find(static_cast<const RE::IAnimationGraphManagerHolder*>(a_holder));
        if (!registration)
        {
            return observation;
        }
        if (a_isJumpLandEnd && registration->landingCompletionObservationAllowed
            && registration->observeJumpLandEndEvent)
        {
            ++registration->pendingJumpLandEnds;
            observation.landingCompletionCount = ++registration->jumpLandEndCount;
            registration->observeJumpLandEndEvent = false;
            observation.actorHandle = registration->actorHandle;
        }
        if (registration->observeWeaponTransitionEndEvent && a_shouldTraceWeaponEvent)
        {
            observation.traceWeaponEvent = true;
            observation.expectedWeaponDrawn = registration->authoritativeWeaponDrawn;
            observation.actorHandle = registration->actorHandle;
        }
        if (registration->observeWeaponTransitionEndEvent
            && IsWeaponTransitionEndEvent(a_eventName, registration->authoritativeWeaponDrawn))
        {
            ++registration->pendingWeaponTransitionEnds;
            observation.weaponCompletionCount = ++registration->weaponTransitionEndCount;
            registration->observeWeaponTransitionEndEvent = false;
            observation.actorHandle = registration->actorHandle;
        }
        if (AsciiCase::Equals(a_eventName, "bashExit"))
        {
            ++registration->pendingBashExits;
            observation.bashExitCount = ++registration->bashExitCount;
            observation.actorHandle = registration->actorHandle;
        }
        return observation;
    }

    std::optional<UpdatePolicy> GetUpdatePolicy(const RE::Actor* a_actor)
    {
        if (!a_actor)
        {
            return std::nullopt;
        }

        const auto lock = LockSharedUnlessMainThread();
        const auto* registration = Find(a_actor);
        if (!registration)
        {
            return std::nullopt;
        }

        return UpdatePolicy{ .authoritativeJumping = registration->authoritativeJumping, .dead = registration->dead };
    }

    void SetAuthoritativeJumpState(const RemoteActor& a_remote, bool a_jumping)
    {
        SetShared(a_remote, &Registration::authoritativeJumping, a_jumping);
    }

    void SetAuthoritativeDirection(const RemoteActor& a_remote)
    {
        SetShared(a_remote, &Registration::authoritativeDirection, a_remote.movement.animationDirection);
    }

    std::optional<float> GetAuthoritativeDirection(const RE::Actor* a_actor)
    {
        if (!a_actor)
        {
            return std::nullopt;
        }

        const auto lock = LockSharedUnlessMainThread();
        const auto* registration = Find(a_actor);
        if (!registration)
        {
            return std::nullopt;
        }
        // A riding gait only travels the way the mount faces.
        return registration->gaitDriven ? 0.0F : registration->authoritativeDirection;
    }

    void SetAuthoritativeWeaponDrawnState(const RemoteActor& a_remote, bool a_weaponDrawn)
    {
        auto lock = LockUnlessMainThread();
        auto* registration = Find(a_remote.animationGraphHolder);
        if (!registration || registration->authoritativeWeaponDrawn == a_weaponDrawn)
        {
            return;
        }
        // The worker observing the completion reads the state, the flag, and the count together.
        LockForWrite(lock);
        registration->authoritativeWeaponDrawn = a_weaponDrawn;
        // Armed before DrawWeaponMagicHands runs on the next update; its completion annotation may be
        // emitted from an animation worker.
        registration->pendingWeaponTransitionEnds = 0;
        registration->observeWeaponTransitionEndEvent = true;
    }

    std::optional<bool> GetAuthoritativeWeaponDrawnState(const RE::Actor& a_actor)
    {
        const auto lock = LockSharedUnlessMainThread();
        const auto* registration = Find(a_actor);
        return registration ? std::optional<bool>{ registration->authoritativeWeaponDrawn } : std::nullopt;
    }

    std::pair<std::uint32_t, std::uint32_t> BlockMagicEffect(const RE::MagicTarget* a_target)
    {
        std::scoped_lock lock{ s_mutex };
        auto* registration = FindIndexed(s_byMagicTarget, a_target);
        if (!registration)
        {
            return {};
        }

        return { registration->actorHandle, ++registration->blockedMagicEffects };
    }

    std::pair<std::uint32_t, std::uint32_t> BlockHealthDamage(const RE::Actor* a_actor)
    {
        if (!a_actor)
        {
            return {};
        }

        std::scoped_lock lock{ s_mutex };
        auto* registration = Find(a_actor);
        if (!registration || registration->dead)
        {
            return {};
        }

        return { registration->actorHandle, ++registration->blockedHealthDamage };
    }

    std::pair<std::uint32_t, std::uint32_t> BlockHealthActorValueDamage(const RE::ActorValueOwner* a_actorValueOwner)
    {
        if (!a_actorValueOwner)
        {
            return {};
        }

        std::scoped_lock lock{ s_mutex };
        auto* registration = FindIndexed(s_byActorValueOwner, a_actorValueOwner);
        if (!registration || registration->dead)
        {
            return {};
        }

        return { registration->actorHandle, ++registration->blockedHealthDamage };
    }

    std::pair<std::uint32_t, std::uint32_t> RequestHitObservation(const RE::Actor* a_actor)
    {
        if (!a_actor)
        {
            return {};
        }

        std::scoped_lock lock{ s_mutex };
        auto* registration = Find(a_actor);
        if (!registration)
        {
            return {};
        }

        ++registration->pendingHitObservations;
        return { registration->actorHandle, ++registration->hitObservationCount };
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
            // Arm only after the grounded transform and graph variables are in place, so a
            // delayed marker from the prior jump cannot complete this one.
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
