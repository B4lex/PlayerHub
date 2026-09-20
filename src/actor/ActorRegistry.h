#pragma once

#include <RE/Skyrim.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

struct RemoteActor;


namespace ActorRegistry
{
    // Main-loop thread only. Registration precedes the first hook observation of
    // a new actor; unregistration precedes deletion of its reference.
    void Register(RemoteActor& a_remote, RE::Actor& a_actor);
    void Unregister(RemoteActor& a_remote);

    [[nodiscard]] bool IsRegistered(const RE::Actor* a_actor);
    // For hooks that only hold a handle; avoids resolving it on a worker thread.
    [[nodiscard]] bool IsRegistered(RE::ActorHandle a_handle);

    // Returns true only when the firewall was newly activated.
    [[nodiscard]] bool EnableAnimationFirewall(const RemoteActor& a_remote);
    void DisableAnimationFirewall(const RemoteActor& a_remote);
    void SetFurnitureAnimationOwned(const RemoteActor& a_remote, bool a_owned);

    void SetCastAnimationOwned(const RemoteActor& a_remote, bool a_owned);
    [[nodiscard]] bool IsCastAnimationOwned(const RE::Actor* a_actor);

    struct GraphInput
    {
        std::uint32_t actorHandle{ 0 };
        bool animationFirewallEnabled{ false };
        bool furnitureAnimationOwned{ false };
        bool castAnimationOwned{ false };
    };

    [[nodiscard]] std::optional<GraphInput> FindGraphInput(const RE::IAnimationGraphManagerHolder* a_holder);
    void RecordSwimTransition(const RE::IAnimationGraphManagerHolder* a_holder, bool a_started);
    [[nodiscard]] std::uint32_t RecordBlockedGraphEvent(
        const RE::IAnimationGraphManagerHolder* a_holder, const char* a_eventName);

    struct GraphOutputObservation
    {
        std::uint32_t actorHandle{ 0 };
        std::uint32_t landingCompletionCount{ 0 };
        std::uint32_t weaponCompletionCount{ 0 };
        std::uint32_t bashExitCount{ 0 };
        bool traceWeaponEvent{ false };
        bool expectedWeaponDrawn{ false };
    };

    [[nodiscard]] GraphOutputObservation ObserveGraphOutputEvent(const RE::TESObjectREFR* a_holder,
        std::string_view a_eventName, bool a_isJumpLandEnd, bool a_shouldTraceWeaponEvent);

    [[nodiscard]] std::optional<bool> GetAuthoritativeJumpState(const RE::Actor* a_actor);
    void SetAuthoritativeJumpState(const RemoteActor& a_remote, bool a_jumping);
    void SetAuthoritativeDirection(const RemoteActor& a_remote);
    [[nodiscard]] std::optional<float> GetAuthoritativeDirection(const RE::Actor* a_actor);
    void SetAuthoritativeWeaponDrawnState(const RemoteActor& a_remote, bool a_weaponDrawn);
    [[nodiscard]] std::optional<bool> GetAuthoritativeWeaponDrawnState(const RE::Actor& a_actor);

    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> BlockMagicEffect(const RE::MagicTarget* a_target);
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> BlockHealthDamage(const RE::Actor* a_actor);
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> BlockHealthActorValueDamage(
        const RE::ActorValueOwner* a_actorValueOwner);
    std::pair<std::uint32_t, std::uint32_t> RequestHitObservation(const RE::Actor* a_actor);
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> RecordCombatSanitization(const RE::Actor& a_actor);

    [[nodiscard]] std::uint32_t ConsumeHitObservations(const RemoteActor& a_remote);
    [[nodiscard]] std::uint32_t ConsumeSwimStarts(const RemoteActor& a_remote);
    [[nodiscard]] std::uint32_t ConsumeSwimStops(const RemoteActor& a_remote);
    [[nodiscard]] std::uint32_t ConsumeWeaponTransitionEnds(const RemoteActor& a_remote);
    [[nodiscard]] std::uint32_t ConsumeJumpLandEnds(const RemoteActor& a_remote);
    [[nodiscard]] std::uint32_t ConsumeCombatSanitizationRequests(const RE::Actor& a_actor);
    [[nodiscard]] std::uint32_t ConsumeBashExits(const RE::Actor& a_actor);

    void SetLandingCompletionObservation(const RE::Actor& a_actor, bool a_allowed);
    void ArmJumpLandEndObservation(const RE::Actor& a_actor);
}
