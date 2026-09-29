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
    // The furniture the actor occupies or is entering, published for the local
    // player's activation, which Skyrim runs on a worker thread.
    void SetHeldFurniture(const RemoteActor& a_remote, RE::FormID a_furnitureRefID);
    [[nodiscard]] bool IsFurnitureHeld(RE::FormID a_furnitureRefID);
    // While set, the graph leaving the actor's furniture node (IdleFurnitureExit)
    // is counted, to tell when a native exit clip has ended.
    void SetNativeFurnitureExit(const RemoteActor& a_remote, bool a_exiting);
    // Safe to call from animation worker threads; takes no lock while no native
    // exit runs.
    void ObserveFurnitureExitGraphOutput(const RE::TESObjectREFR* a_holder, std::string_view a_eventName);
    [[nodiscard]] std::uint32_t ConsumeFurnitureExitClipEnds(const RemoteActor& a_remote);

    void SetCastAnimationOwned(const RemoteActor& a_remote, bool a_owned);
    [[nodiscard]] bool IsCastAnimationOwned(const RE::Actor* a_actor);

    // The actor's player died. For the rest of the registration the actor is
    // left to Skyrim's own death: damage and kills reach it, survival is no
    // longer enforced, and every graph event is admitted.
    void SetDead(const RemoteActor& a_remote);
    // The actor rides a remote mount, whose native interaction owns its graph.
    void SetMounted(const RemoteActor& a_remote, bool a_mounted);
    // Marks a remote mount or its seated rider as having its riding gait driven
    // by the network, and sets the speed its graph's Speed channel reports.
    // Skyrim polls that channel from the actor's own movement, which is zero for
    // a mount moved by its transform and for a rider it carries.
    void SetGaitSpeed(const RemoteActor& a_remote, float a_speed);
    // While set, Skyrim's movement update is skipped for the actor, so the
    // network transform is the only writer of its position and heading.
    void SetNetworkPlaced(const RemoteActor& a_remote, bool a_networkPlaced);
    // Whether a living actor is network-placed. Safe to call from the movement
    // update's worker threads.
    [[nodiscard]] bool IsNetworkPlaced(const RE::Actor* a_actor);
    // The graph Speed of a network-placed actor. Skyrim's Speed channel measures
    // it from the movement update, which such an actor no longer runs.
    void SetAuthoritativeSpeed(const RemoteActor& a_remote, float a_speed);
    // The speed a gait-driven actor, or else a living network-placed actor,
    // reports to its graph, or nullopt for any other actor. Safe to call from
    // animation worker threads.
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

    // What the engine actor update must reassert, or nullopt for an actor that
    // is not a remote actor.
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
