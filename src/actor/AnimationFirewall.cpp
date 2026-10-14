#include "actor/AnimationFirewall.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRegistry.h"
#include "actor/AnimationEvents.h"
#include "actor/RemoteActor.h"
#include "runtime/Profiling.h"
#include "text/AsciiCase.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include <string_view>

namespace AnimationFirewall
{
    namespace
    {
        using ActorConstants::ShouldLogOccurrence;
        using AnimationEvents::IsCastAnimationEvent;
        using AnimationEvents::IsWeaponTransitionEndEvent;
        using AnimationEvents::IsWeaponTransitionTraceEvent;

        constexpr bool IsClientManagedAnimationEvent(std::string_view a_eventName) noexcept
        {
            return a_eventName == "SwimStart" || a_eventName == "swimStop" || a_eventName == "WeapEquip"
                || a_eventName == "WeapUnequip" || a_eventName == "Equip" || a_eventName == "Unequip" ||
                // Equipping over a drawn weapon plays this quick equip, whose weaponDraw
                // annotation moves the new weapon from its sheath node into the hand.
                a_eventName == "WeapOutRightReplaceForceEquip" || a_eventName == "WeapOutLeftReplaceForceEquip" ||
                // Drawing spell hands, and equipping a spell or weapon over them.
                a_eventName == "Magic_Equip" || a_eventName == "MagicForceEquip"
                || a_eventName == "MagicForceEquipRight" || a_eventName == "MagicForceEquipLeft"
                || a_eventName == "MagicWeap_ForceEquip" ||
                // ActionForceEquip emptying the left hand beside a weapon or unarmed
                // right hand, or beside a spell (the LeftH2HEquip and
                // LeftMagicForceUnequip idles).
                a_eventName == "WeapSoloEquip" || a_eventName == "Magic_Solo_Equip" ||
                // Raises and lowers the left arm for a carried torch.
                a_eventName == "torchEquip" || a_eventName == "torchUnequip";
        }

        // Locomotion requests Skyrim's own movement logic may send a remote mount.
        constexpr bool IsMountGaitEvent(std::string_view a_eventName) noexcept
        {
            using AsciiCase::Equals;
            return Equals(a_eventName, "moveStart") || Equals(a_eventName, "moveStop")
                || Equals(a_eventName, "SprintStart") || Equals(a_eventName, "SprintStop")
                || Equals(a_eventName, "moveForward") || Equals(a_eventName, "moveBackward")
                || Equals(a_eventName, "BrakeStart") || Equals(a_eventName, "BrakeStop")
                || Equals(a_eventName, "turnLeft") || Equals(a_eventName, "turnRight")
                || Equals(a_eventName, "turnStop") || AsciiCase::StartsWith(a_eventName, "cannedTurn");
        }

        static_assert(IsMountGaitEvent("MoveStop"));
        static_assert(IsMountGaitEvent("cannedTurnLeft90"));
        static_assert(!IsMountGaitEvent("HorseEnter"));

        static_assert(IsClientManagedAnimationEvent("swimStop"));
        static_assert(IsClientManagedAnimationEvent("SwimStart"));
        static_assert(IsClientManagedAnimationEvent("WeapEquip"));
        static_assert(IsClientManagedAnimationEvent("WeapUnequip"));
        static_assert(!IsClientManagedAnimationEvent("AttackStart"));

        class GraphOutputEventSink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
        {
        public:
            static GraphOutputEventSink* GetSingleton()
            {
                static GraphOutputEventSink singleton;
                return std::addressof(singleton);
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
            {
                const auto* eventName = a_event ? a_event->tag.c_str() : nullptr;
                if (!a_event || !a_event->holder || !eventName)
                {
                    return RE::BSEventNotifyControl::kContinue;
                }
                ActorRegistry::ObserveFurnitureExitGraphOutput(a_event->holder, eventName);
                const std::string_view eventView{ eventName };
                const bool isJumpLandEnd = eventView == "JumpLandEnd";
                const bool isWeaponTransitionEnd
                    = IsWeaponTransitionEndEvent(eventView, true) || IsWeaponTransitionEndEvent(eventView, false);
                const bool shouldTraceWeaponEvent = IsWeaponTransitionTraceEvent(eventView);
                const bool isBashExit = AsciiCase::Equals(eventView, "bashExit");
                if (!isJumpLandEnd && !isWeaponTransitionEnd && !shouldTraceWeaponEvent && !isBashExit)
                {
                    return RE::BSEventNotifyControl::kContinue;
                }

                const auto observation = ActorRegistry::ObserveGraphOutputEvent(
                    a_event->holder, eventView, isJumpLandEnd, shouldTraceWeaponEvent);
                const auto actorHandle = observation.actorHandle;
                const auto landingCompletionCount = observation.landingCompletionCount;
                const auto weaponCompletionCount = observation.weaponCompletionCount;
                const auto traceWeaponEvent = observation.traceWeaponEvent;
                const auto expectedWeaponDrawn = observation.expectedWeaponDrawn;

                if (landingCompletionCount != 0)
                {
                    logs::debug("Observed actor landing completion marker: actor={:08X} count={}", actorHandle,
                        landingCompletionCount);
                }
                if (traceWeaponEvent)
                {
                    logs::debug(
                        "Observed actor weapon-transition graph event: actor={:08X} expectedWeaponDrawn={} event={}",
                        actorHandle, expectedWeaponDrawn, eventName);
                }
                if (observation.bashExitCount != 0)
                {
                    logs::debug("Observed actor bash exit marker: actor={:08X} count={}", actorHandle,
                        observation.bashExitCount);
                }
                if (weaponCompletionCount != 0)
                {
                    logs::debug("Observed actor weapon transition completion marker: actor={:08X} event={} count={}",
                        actorHandle, eventName, weaponCompletionCount);
                }
                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            GraphOutputEventSink() = default;
        };

        struct GraphInputHook
        {
            static bool Thunk(RE::IAnimationGraphManagerHolder* a_holder, const RE::BSFixedString& a_eventName)
            {
                std::uint32_t actorHandle = 0;
                std::uint32_t blockedEventCount = 0;
                bool firewallEnabled = false;
                bool allowClientManagedEvent = false;
                bool allowNativeGraphEvent = false;
                bool allowCastEvent = false;
                bool isSwimStartEvent = false;
                bool isSwimStopEvent = false;
                bool blockMountGaitEvent = false;
                const auto* eventName = a_eventName.c_str();
                if (const auto registration = ActorRegistry::FindGraphInput(a_holder))
                {
                    firewallEnabled = registration->animationFirewallEnabled;
                    actorHandle = registration->actorHandle;
                    const std::string_view eventView = eventName ? std::string_view{ eventName } : std::string_view{};
                    // Weapon draw/sheathe requests can reach this hook from an
                    // animation worker after the authoritative request was queued.
                    allowClientManagedEvent = IsClientManagedAnimationEvent(eventView);
                    allowNativeGraphEvent
                        = registration->furnitureAnimationOwned || registration->dead || registration->mounted;
                    allowCastEvent = registration->castAnimationOwned && IsCastAnimationEvent(eventView);
                    // A seated rider's riding graph follows its mount.
                    blockMountGaitEvent
                        = registration->gaitDriven && !registration->mounted && IsMountGaitEvent(eventView);
                    isSwimStartEvent = eventView == "SwimStart";
                    isSwimStopEvent = eventView == "swimStop";
                }

                // Filtering graph construction and do-nothing package setup events can
                // leave a newly spawned actor in its default pose. Furniture event
                // names vary by furniture record and are generated locally by Skyrim.
                if (!blockMountGaitEvent
                    && (!firewallEnabled || allowClientManagedEvent || allowNativeGraphEvent || allowCastEvent))
                {
                    const bool accepted = _original(a_holder, a_eventName);
                    if (firewallEnabled && (isSwimStartEvent || isSwimStopEvent))
                    {
                        if (accepted)
                        {
                            ActorRegistry::RecordSwimTransition(a_holder, isSwimStartEvent);
                        }
                        logs::debug("Observed actor swim transition: actor={:08X} event={} accepted={}", actorHandle,
                            eventName, accepted);
                    }
                    return accepted;
                }

                blockedEventCount = ActorRegistry::RecordBlockedGraphEvent(a_holder, eventName);

                if (blockedEventCount != 0 && ShouldLogOccurrence(blockedEventCount))
                {
                    logs::debug("Blocked local actor animation event: actor={:08X} event={} count={}", actorHandle,
                        a_eventName.c_str(), blockedEventCount);
                }

                return false;
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // BSTAnimationGraphDataChannel<Actor, float, Channel>:
        // BSAnimationGraphChannel::value at 0x18, then the channel's cached value
        // at 0x24 and its Actor at 0x28 (see RE::ActorSpeedChannel).
        constexpr std::ptrdiff_t kChannelGraphValueOffset = 0x18;
        constexpr std::ptrdiff_t kChannelCachedValueOffset = 0x24;
        constexpr std::ptrdiff_t kChannelActorOffset = 0x28;

        const RE::Actor* GetChannelActor(const RE::BSAnimationGraphChannel* a_channel)
        {
            const RE::Actor* actor = nullptr;
            std::memcpy(&actor, reinterpret_cast<const std::byte*>(a_channel) + kChannelActorOffset, sizeof(actor));
            return actor;
        }

        void OverrideChannelValue(RE::BSAnimationGraphChannel* a_channel, float a_value)
        {
            auto* bytes = reinterpret_cast<std::byte*>(a_channel);
            std::memcpy(bytes + kChannelGraphValueOffset, &a_value, sizeof(float));
            std::memcpy(bytes + kChannelCachedValueOffset, &a_value, sizeof(float));
        }

        // Skyrim copies Actor-derived values into graph variables through channels
        // polled during each animation update, after main-loop graph writes. The
        // Direction channel reads the remote actor's own movement request, which is always
        // forward, so replace its polled value with the network direction.
        struct DirectionChannelHook
        {
            static void Thunk(RE::BSAnimationGraphChannel* a_channel, bool a_arg)
            {
                _original(a_channel, a_arg);
                if (!a_channel)
                {
                    return;
                }

                if (const auto direction = ActorRegistry::GetAuthoritativeDirection(GetChannelActor(a_channel)))
                {
                    OverrideChannelValue(a_channel, *direction);
                }
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // The Speed channel reads the speed Skyrim's movement update measures. A
        // network-placed actor skips that update, and a rider is carried by its
        // mount, so their locomotion would play at a stale speed or standing still.
        struct SpeedChannelHook
        {
            static void Thunk(RE::BSAnimationGraphChannel* a_channel, bool a_arg)
            {
                _original(a_channel, a_arg);
                if (!a_channel)
                {
                    return;
                }

                if (const auto speed = ActorRegistry::GetAuthoritativeSpeed(GetChannelActor(a_channel)))
                {
                    OverrideChannelValue(a_channel, *speed);
                }
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // The TurnDelta channel follows the local AI's turn request, which the network
        // heading never satisfies. It sets the speed of the turn-in-place blends and
        // first-person rotation.
        struct TurnDeltaChannelHook
        {
            static void Thunk(RE::BSAnimationGraphChannel* a_channel, bool a_arg)
            {
                _original(a_channel, a_arg);
                if (a_channel && ActorRegistry::IsRegistered(GetChannelActor(a_channel)))
                {
                    OverrideChannelValue(a_channel, 0.0F);
                }
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // The weapon speed channels feed weaponSpeedMult and leftWeaponSpeedMult,
        // which scale the playback of the vanilla attack clips (hkbClipGenerator
        // playbackSpeed bindings in the behavior files). Unlike the Speed and
        // Direction channels they keep no cached value: the Actor is at 0x20, and
        // vfunc 1 rewrites the value at 0x18 from the actor on every poll (1.7.104
        // disassembly), so scaling it there cannot compound.
        constexpr std::ptrdiff_t kWeaponSpeedChannelActorOffset = 0x20;

        template <class Tag>
        struct WeaponSpeedChannelHook
        {
            static void Thunk(RE::BSAnimationGraphChannel* a_channel, bool a_arg)
            {
                _original(a_channel, a_arg);
                if (!a_channel)
                {
                    return;
                }

                auto* bytes = reinterpret_cast<std::byte*>(a_channel);
                const RE::Actor* actor = nullptr;
                std::memcpy(&actor, bytes + kWeaponSpeedChannelActorOffset, sizeof(actor));
                if (const auto scale = ActorRegistry::GetAttackSpeedScale(actor))
                {
                    float value = 0.0F;
                    std::memcpy(&value, bytes + kChannelGraphValueOffset, sizeof(value));
                    value *= *scale;
                    std::memcpy(bytes + kChannelGraphValueOffset, &value, sizeof(value));
                }
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        struct RightWeaponSpeedTag;
        struct LeftWeaponSpeedTag;
    }

    void InstallHooks()
    {
        // NPC references are Character instances. Their first four vtables belong
        // to TESObjectREFR's four polymorphic bases. Index 3 is the
        // IAnimationGraphManagerHolder subobject, whose vfunc 1 is
        // NotifyAnimationGraph.
        REL::Relocation<std::uintptr_t> characterAnimationGraphVTable{ RE::VTABLE_Character[3] };
        GraphInputHook::_original = characterAnimationGraphVTable.write_vfunc(0x1, GraphInputHook::Thunk);
        REL::Relocation<std::uintptr_t> directionChannelVTable{
            RE::VTABLE_BSTAnimationGraphDataChannel_Actor_float_ActorDirectionChannel_[0]
        };
        DirectionChannelHook::_original = directionChannelVTable.write_vfunc(0x1, DirectionChannelHook::Thunk);
        REL::Relocation<std::uintptr_t> speedChannelVTable{
            RE::VTABLE_BSTAnimationGraphDataChannel_Actor_float_ActorSpeedChannel_[0]
        };
        SpeedChannelHook::_original = speedChannelVTable.write_vfunc(0x1, SpeedChannelHook::Thunk);
        REL::Relocation<std::uintptr_t> turnDeltaChannelVTable{
            RE::VTABLE_BSTAnimationGraphDataChannel_Actor_float_ActorTurnDeltaChannel_[0]
        };
        TurnDeltaChannelHook::_original = turnDeltaChannelVTable.write_vfunc(0x1, TurnDeltaChannelHook::Thunk);
        REL::Relocation<std::uintptr_t> weaponSpeedChannelVTable{
            RE::VTABLE_BSTAnimationGraphDataChannel_Actor_float_ActorWeaponSpeedChannel_[0]
        };
        WeaponSpeedChannelHook<RightWeaponSpeedTag>::_original
            = weaponSpeedChannelVTable.write_vfunc(0x1, WeaponSpeedChannelHook<RightWeaponSpeedTag>::Thunk);
        REL::Relocation<std::uintptr_t> leftWeaponSpeedChannelVTable{
            RE::VTABLE_BSTAnimationGraphDataChannel_Actor_float_ActorLeftWeaponSpeedChannel_[0]
        };
        WeaponSpeedChannelHook<LeftWeaponSpeedTag>::_original
            = leftWeaponSpeedChannelVTable.write_vfunc(0x1, WeaponSpeedChannelHook<LeftWeaponSpeedTag>::Thunk);
    }

    void Enable(RemoteActor& a_remote)
    {
        const bool activated = ActorRegistry::EnableAnimationFirewall(a_remote);

        if (activated)
        {
            logs::debug("Activated actor animation firewall: actor={:08X}", a_remote.handle.native_handle());
        }
    }

    void Disable(RemoteActor& a_remote)
    {
        ActorRegistry::DisableAnimationFirewall(a_remote);
    }

    bool RegisterGraphOutputEventSink(RE::BSAnimationGraphManager& a_graphManager)
    {
        if (a_graphManager.graphs.empty() || !a_graphManager.graphs.front())
        {
            return false;
        }

        // Animation annotations are emitted by the graph's event source. They do
        // not pass through IAnimationGraphManagerHolder::NotifyAnimationGraph,
        // which is the input path filtered by GraphInputHook.
        a_graphManager.graphs.front()->GetEventSource<RE::BSAnimationGraphEvent>()->AddEventSink(
            GraphOutputEventSink::GetSingleton());
        return true;
    }

    bool NotifyNetworkAnimationGraph(RE::Actor& a_actor, const RE::BSFixedString& a_eventName)
    {
        // Calling the saved vfunc directly skips the filter at the public dispatch point,
        // where local game and mod events arrive.
        auto* holder = static_cast<RE::IAnimationGraphManagerHolder*>(std::addressof(a_actor));
        return GraphInputHook::_original(holder, a_eventName);
    }

    bool ForceDefaultGraphState(RE::Actor& a_actor)
    {
        PROFILE_FUNCTION();
        static const RE::BSFixedString defaultStateEvent{ "IdleForceDefaultState" };
        static const RE::BSFixedString torchEquipEvent{ "torchEquip" };
        const bool defaultStateAccepted = NotifyNetworkAnimationGraph(a_actor, defaultStateEvent);
        const auto* leftHand = a_actor.GetEquippedObject(true);
        const auto* light = leftHand ? leftHand->As<RE::TESObjectLIGH>() : nullptr;
        if (defaultStateAccepted && light && light->CanBeCarried())
        {
            const bool torchAccepted = NotifyNetworkAnimationGraph(a_actor, torchEquipEvent);
            logs::debug("Raised carried torch after graph default reset: actor={:08X} torch={:08X} accepted={}",
                a_actor.GetHandle().native_handle(), light->GetFormID(), torchAccepted);
        }
        return defaultStateAccepted;
    }
}
