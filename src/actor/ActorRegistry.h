#pragma once

#include <RE/Skyrim.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

struct RemoteActor;


namespace ActorRegistry
{
    void Register(RemoteActor& a_remote, RE::Actor& a_actor);
    void Unregister(RemoteActor& a_remote);

    [[nodiscard]] bool IsRegistered(const RE::Actor* a_actor);
    [[nodiscard]] bool IsRegistered(RE::ActorHandle a_handle);

    [[nodiscard]] bool EnableAnimationFirewall(const RemoteActor& a_remote);
    void DisableAnimationFirewall(const RemoteActor& a_remote);
    void SetFurnitureAnimationOwned(const RemoteActor& a_remote, bool a_owned);
    void SetHeldFurniture(const RemoteActor& a_remote, RE::FormID a_furnitureRefID);
    [[nodiscard]] bool IsFurnitureHeld(RE::FormID a_furnitureRefID);
    void SetNativeFurnitureExit(const RemoteActor& a_remote, bool a_exiting);
    void ObserveFurnitureExitGraphOutput(const RE::TESObjectREFR* a_holder, std::string_view a_eventName);
    [[nodiscard]] std::uint32_t ConsumeFurnitureExitClipEnds(const RemoteActor& a_remote);

    void SetCastAnimationOwned(const RemoteActor& a_remote, bool a_owned);
    [[nodiscard]] bool IsCastAnimationOwned(const RE::Actor* a_actor);

    void SetDead(const RemoteActor& a_remote);
    void SetMounted(const RemoteActor& a_remote, bool a_mounted);
    // Skyrim polls the Speed channel from the actor's own movement, which is zero
    // for a mount moved by its transform and for a rider it carries.
    void SetGaitSpeed(const RemoteActor& a_remote, float a_speed);
    void SetNetworkPlaced(const RemoteActor& a_remote, bool a_networkPlaced);
    [[nodiscard]] bool IsNetworkPlaced(const RE::Actor* a_actor);
    // Scales the actor's polled weapon speeds while its attacks catch up with the
    // source; 1 leaves them to Skyrim.
    void SetAttackSpeedScale(const RemoteActor& a_remote, float a_scale);
    [[nodiscard]] std::optional<float> GetAttackSpeedScale(const RE::Actor* a_actor);
    void SetAuthoritativeSpeed(const RemoteActor& a_remote, float a_speed);
    [[nodiscard]] std::optional<float> GetAuthoritativeSpeed(const RE::Actor* a_actor);

    struct GraphInput
    {
        std::uint32_t actorHandle{ 0 };
        bool animationFirewallEnabled{ false };
        bool furnitureAnimationOwned{ false };
        bool castAnimationOwned{ false };
        bool dead{ false };
        bool mounted{ false };
        bool gaitDriven{ false };
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

    struct UpdatePolicy
    {
        bool authoritativeJumping{ false };
        bool dead{ false };
    };

    [[nodiscard]] std::optional<UpdatePolicy> GetUpdatePolicy(const RE::Actor* a_actor);
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
