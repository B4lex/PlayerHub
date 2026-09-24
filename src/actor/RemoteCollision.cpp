#include "actor/RemoteCollision.h"
#include "actor/ActorRegistry.h"
#include "actor/RemoteActor.h"

#include <cstdint>
#include <memory>

namespace RemoteCollision
{
    namespace
    {
        // Skyrim's movement update (ID 37356) moves an actor by its animation's
        // root motion through its character controller, which pushes out of and
        // pushes local bodies, and then feeds the achieved motion back into the
        // graph. For a network-placed actor all of that fights the network
        // transform, so it is skipped. Otherwise, as in powerofthree's Tweaks' TCL
        // fix, the character body a moving controller bumped into ignores
        // collision for that update when either character is remote.
        struct UpdateMovementHook
        {
            // The bumped body's filter to disable, or null when the mover and the
            // bumped character are both local.
            static RE::CFilter* GetPassThroughFilter(RE::Actor* a_mover)
            {
                auto* controller = a_mover ? a_mover->GetCharController() : nullptr;
                auto* bumped = controller ? controller->bumpedCharCollisionObject.get() : nullptr;
                if (!bumped)
                {
                    return nullptr;
                }

                auto& filter = bumped->collidable.broadPhaseHandle.collisionFilterInfo;
                if (filter.QNoCollision())
                {
                    return nullptr;
                }
                if (ActorRegistry::IsRegistered(a_mover))
                {
                    return std::addressof(filter);
                }
                auto* bumpedRef = RE::TESHavokUtilities::FindCollidableRef(bumped->collidable);
                return bumpedRef && ActorRegistry::IsRegistered(bumpedRef->As<RE::Actor>()) ? std::addressof(filter)
                                                                                            : nullptr;
            }

            static void Thunk(RE::Actor* a_actor, float a_delta)
            {
                if (ActorRegistry::IsNetworkPlaced(a_actor))
                {
                    return;
                }

                auto* filter = GetPassThroughFilter(a_actor);
                if (filter)
                {
                    filter->SetNoCollision(true);
                }
                _original(a_actor, a_delta);
                if (filter)
                {
                    filter->SetNoCollision(false);
                }
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        void SetNetworkPlaced(RemoteActor& a_remote, bool a_networkPlaced, std::string_view a_owner)
        {
            if (a_remote.collision.networkPlaced == a_networkPlaced)
            {
                return;
            }
            a_remote.collision.networkPlaced = a_networkPlaced;
            ActorRegistry::SetNetworkPlaced(a_remote, a_networkPlaced);
            logs::info("Remote actor placement owner changed: actor={:08X} owner={} engineMovement={}",
                a_remote.handle.native_handle(), a_owner, !a_networkPlaced);
        }

        // Skyrim gives every actor but the player a rigid-body controller (ID
        // 37171), whose character body is also its world collision: a body the
        // physics step simulates, which collides with and pushes local bodies and
        // triggers. It needs its collision only while Skyrim moves the actor.
        void SetCharacterBodyIsolated(RE::Actor& a_actor, bool a_isolated, std::string_view a_owner)
        {
            auto* controller = a_actor.GetCharController();
            auto* body = controller ? controller->GetRigidBody() : nullptr;
            if (!body)
            {
                return;
            }
            auto& filter = body->collidable.broadPhaseHandle.collisionFilterInfo;
            if (filter.QNoCollision() == a_isolated)
            {
                return;
            }
            filter.SetNoCollision(a_isolated);
            logs::info("Remote actor character body collision {}: actor={:08X} owner={} controller={}",
                a_isolated ? "disabled" : "restored", a_actor.GetHandle().native_handle(), a_owner,
                skyrim_cast<RE::bhkCharProxyController*>(controller) ? "proxy" : "rigidBody");
        }

        // Every physics step, the rigid-body controller's post-simulation callback
        // (ID 79185) sends its body's position, with the body's rotation applied
        // to its center offset, to the actor's move-finish sink, which places the
        // actor there, unless kNoSim is set. Only the skipped update keeps that
        // body upright and on the actor, so a tumbling body would carry the actor
        // around a circle. Skyrim's own uses of kNoSim restore it afterwards.
        void SetBodyPlacesActor(RE::Actor& a_actor, bool a_placesActor)
        {
            auto* controller = a_actor.GetCharController();
            if (!controller || controller->flags.none(RE::CHARACTER_FLAGS::kNoSim) == a_placesActor)
            {
                return;
            }
            controller->flags.set(!a_placesActor, RE::CHARACTER_FLAGS::kNoSim);
            logs::info("Remote actor character body {} the actor: actor={:08X}",
                a_placesActor ? "places" : "no longer places", a_actor.GetHandle().native_handle());
        }

        // Only the skipped update's controller integration measures support.
        // Skyrim ends swimming in the actor update (ID 37348) once the actor is
        // submerged below 70% of its height and its controller is supported; the
        // unsupported path waits for an in-air state that integration never applies.
        void ReportSupport(RE::Actor& a_actor, const RemoteActor& a_remote)
        {
            auto* controller = a_actor.GetCharController();
            if (!controller)
            {
                return;
            }
            const bool supported = !a_remote.movement.sourceSwimming;
            if (controller->flags.all(RE::CHARACTER_FLAGS::kSupport) == supported)
            {
                return;
            }
            controller->flags.set(supported, RE::CHARACTER_FLAGS::kSupport);
            const auto* actorState = a_actor.AsActorState();
            logs::info("Remote actor controller support changed: actor={:08X} supported={} swimming={}",
                a_remote.handle.native_handle(), supported, actorState && actorState->IsSwimming());
        }
    }

    void InstallHooks()
    {
        // The call site is verified against 1.7.104 only.
        if (!REL::Module::IsAE())
        {
            logs::warn(
                "Remote actor movement isolation is verified only on Skyrim AE; the local world may push remote "
                "actors, and remote actors may body-block characters");
            return;
        }

        // Actor movement update call (ID 37350 + 0xFB on 1.7.104), its only caller,
        // as patched by powerofthree's Tweaks. write_call chains onto another
        // plugin's detour.
        constexpr std::ptrdiff_t kUpdateMovementCallOffset = 0xFB;
        const auto site = REL::Relocation<std::uintptr_t>{ REL::ID(37350) }.address() + kUpdateMovementCallOffset;
        if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8)
        {
            logs::error("Did not install remote actor movement isolation hook: no call at ID 37350 + {:#x}",
                kUpdateMovementCallOffset);
            return;
        }
        UpdateMovementHook::_original
            = SKSE::GetTrampoline().write_call<5>(site, reinterpret_cast<std::uintptr_t>(&UpdateMovementHook::Thunk));
        logs::info("Installed remote actor movement isolation hook");
    }

    void PlaceByNetwork(RE::Actor& a_actor, RemoteActor& a_remote)
    {
        SetNetworkPlaced(a_remote, true, "network");
        SetBodyPlacesActor(a_actor, false);
        SetCharacterBodyIsolated(a_actor, true, "network");
        ReportSupport(a_actor, a_remote);
    }

    void ReleaseToEngine(RE::Actor& a_actor, RemoteActor& a_remote, std::string_view a_owner, bool a_carried)
    {
        // Only on the change: Skyrim may set kNoSim itself while it owns the actor.
        if (a_remote.collision.networkPlaced)
        {
            SetBodyPlacesActor(a_actor, true);
        }
        SetNetworkPlaced(a_remote, false, a_owner);
        SetCharacterBodyIsolated(a_actor, a_carried, a_owner);
    }
}
