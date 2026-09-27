#include "actor/RemoteCollision.h"
#include "actor/ActorConstants.h"
#include "actor/ActorRegistry.h"
#include "actor/RemoteActor.h"
#include "runtime/Profiling.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace RemoteCollision
{
    namespace
    {
        // Collision filters a mover ignores for one movement update, restored
        // when it ends. Skipped when already ignored, so each is restored once.
        class PassThrough
        {
        public:
            PassThrough() = default;
            PassThrough(const PassThrough&) = delete;
            PassThrough& operator=(const PassThrough&) = delete;

            ~PassThrough()
            {
                for (std::size_t index = 0; index < _count; ++index)
                {
                    _filters[index]->SetNoCollision(false);
                }
            }

            void Add(RE::CFilter& a_filter)
            {
                if (a_filter.QNoCollision() || _count == _filters.size())
                {
                    return;
                }
                a_filter.SetNoCollision(true);
                _filters[_count++] = std::addressof(a_filter);
            }

        private:
            std::array<RE::CFilter*, 64> _filters{};
            std::size_t _count{ 0 };
        };

        bool IsCharacterLayer(RE::COL_LAYER a_layer)
        {
            return a_layer == RE::COL_LAYER::kCharController || a_layer == RE::COL_LAYER::kBiped
                || a_layer == RE::COL_LAYER::kBipedNoCC;
        }

        // Skyrim's movement update (ID 37356) moves an actor by its animation's
        // root motion through its character controller, which pushes out of and
        // pushes local bodies, and then feeds the achieved motion back into the
        // graph. For a network-placed actor all of that fights the network
        // transform, so it is skipped. Otherwise, as in powerofthree's Tweaks' TCL
        // fix, the character body a moving controller bumped into ignores
        // collision for that update when either character is remote.
        //
        // The player's proxy controller also collides with remote skeleton
        // bodies, such as a seated rider's legs at its mount's flanks. Its
        // phantom's overlap list holds every body its movement can touch, so the
        // remote character and skeleton bodies in it ignore collision for the
        // player's update, and keep it for everything else.
        struct UpdateMovementHook
        {
            static void AddBumpedCharacter(RE::Actor* a_mover, PassThrough& a_passThrough)
            {
                auto* controller = a_mover ? a_mover->GetCharController() : nullptr;
                auto* bumped = controller ? controller->bumpedCharCollisionObject.get() : nullptr;
                if (!bumped)
                {
                    return;
                }
                if (ActorRegistry::IsRegistered(a_mover))
                {
                    a_passThrough.Add(bumped->collidable.broadPhaseHandle.collisionFilterInfo);
                    return;
                }
                auto* bumpedRef = RE::TESHavokUtilities::FindCollidableRef(bumped->collidable);
                if (bumpedRef && ActorRegistry::IsRegistered(bumpedRef->As<RE::Actor>()))
                {
                    a_passThrough.Add(bumped->collidable.broadPhaseHandle.collisionFilterInfo);
                }
            }

            static void AddRemoteBody(RE::hkpCollidable* a_collidable, PassThrough& a_passThrough)
            {
                if (!a_collidable)
                {
                    return;
                }
                auto& filter = a_collidable->broadPhaseHandle.collisionFilterInfo;
                if (filter.QNoCollision() || !IsCharacterLayer(filter.GetCollisionLayer()))
                {
                    return;
                }
                auto* ref = RE::TESHavokUtilities::FindCollidableRef(*a_collidable);
                if (ref && ActorRegistry::IsRegistered(ref->As<RE::Actor>()))
                {
                    a_passThrough.Add(filter);
                }
            }

            static void AddOverlappingRemoteBodies(RE::Actor& a_player, PassThrough& a_passThrough)
            {
                auto* controller = skyrim_cast<RE::bhkCharProxyController*>(a_player.GetCharController());
                auto* proxy = controller ? controller->GetCharacterProxy() : nullptr;
                auto* phantom = proxy ? proxy->shapePhantom : nullptr;
                if (!phantom)
                {
                    return;
                }
                switch (phantom->GetType())
                {
                    case RE::hkpPhantomType::kSimpleShape:
                        for (auto* collidable : static_cast<RE::hkpSimpleShapePhantom*>(phantom)->collisionDetails)
                        {
                            AddRemoteBody(collidable, a_passThrough);
                        }
                        break;
                    case RE::hkpPhantomType::kCachingShape:
                        for (auto& detail : static_cast<RE::hkpCachingShapePhantom*>(phantom)->collisionDetails)
                        {
                            AddRemoteBody(detail.collidable, a_passThrough);
                        }
                        break;
                    default:
                    {
                        static std::atomic_flag s_reported;
                        if (!s_reported.test_and_set())
                        {
                            logs::warn(
                                "Local player phantom has an unsupported type {}; remote skeletons may "
                                "body-block the player",
                                std::to_underlying(phantom->GetType()));
                        }
                        return;
                    }
                }
            }

            static void Thunk(RE::Actor* a_actor, float a_delta)
            {
                if (ActorRegistry::IsNetworkPlaced(a_actor))
                {
                    return;
                }

                PassThrough passThrough;
                AddBumpedCharacter(a_actor, passThrough);
                if (a_actor && a_actor->IsPlayerRef())
                {
                    AddOverlappingRemoteBodies(*a_actor, passThrough);
                }
                _original(a_actor, a_delta);
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
            logs::debug("Remote actor placement owner changed: actor={:08X} owner={} engineMovement={}",
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
            logs::debug("Remote actor character body collision {}: actor={:08X} owner={} controller={}",
                a_isolated ? "disabled" : "restored", a_actor.GetHandle().native_handle(), a_owner,
                skyrim_cast<RE::bhkCharProxyController*>(controller) ? "proxy" : "rigidBody");
        }

        constexpr float kSkeletonRecheckSeconds = 0.5F;

        // A live skeleton's bone and weapon bodies, and a dead one's ragdoll.
        bool IsSkeletonLayer(RE::COL_LAYER a_layer)
        {
            return a_layer == RE::COL_LAYER::kBiped || a_layer == RE::COL_LAYER::kBipedNoCC
                || a_layer == RE::COL_LAYER::kDeadBip;
        }

        // Returns how many bodies changed.
        std::uint32_t SetSkeletonIsolated(RE::Actor& a_actor, bool a_isolated)
        {
            auto* root = a_actor.Get3D(false);
            if (!root)
            {
                return 0;
            }
            std::uint32_t changed = 0;
            RE::BSVisit::TraverseScenegraphCollision(root, [&](RE::bhkNiCollisionObject* a_object) {
                auto* body = a_object->body.get();
                auto* worldObject = body ? static_cast<RE::hkpWorldObject*>(body->referencedObject.get()) : nullptr;
                if (!worldObject)
                {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }
                auto& filter = worldObject->collidable.broadPhaseHandle.collisionFilterInfo;
                if (IsSkeletonLayer(filter.GetCollisionLayer()) && filter.QNoCollision() != a_isolated)
                {
                    filter.SetNoCollision(a_isolated);
                    ++changed;
                }
                return RE::BSVisit::BSVisitControl::kContinue;
            });
            return changed;
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
            logs::debug("Remote actor character body {} the actor: actor={:08X}",
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
            logs::debug("Remote actor controller support changed: actor={:08X} supported={} swimming={}",
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
        logs::debug("Installed remote actor movement isolation hook");
    }

    void PlaceByNetwork(RE::Actor& a_actor, RemoteActor& a_remote)
    {
        PROFILE_FUNCTION();
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
        if (!a_carried && a_remote.collision.skeletonIsolated)
        {
            a_remote.collision.skeletonIsolated = false;
            const auto changed = SetSkeletonIsolated(a_actor, false);
            logs::debug("Remote actor skeleton collision restored: actor={:08X} owner={} bodies={}",
                a_remote.handle.native_handle(), a_owner, changed);
        }
    }

    void IsolateCarriedSkeleton(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime)
    {
        auto& collision = a_remote.collision;
        if (collision.skeletonIsolated)
        {
            collision.skeletonRecheckElapsed += std::clamp(a_deltaTime, 0.0F, ActorConstants::kMaximumFrameTime);
            if (collision.skeletonRecheckElapsed < kSkeletonRecheckSeconds)
            {
                return;
            }
        }
        collision.skeletonRecheckElapsed = 0.0F;

        PROFILE_FUNCTION();
        const auto changed = SetSkeletonIsolated(a_actor, true);
        if (!collision.skeletonIsolated || changed > 0)
        {
            logs::debug("Remote actor skeleton collision {}: actor={:08X} bodies={}",
                collision.skeletonIsolated ? "reasserted" : "disabled", a_remote.handle.native_handle(), changed);
        }
        collision.skeletonIsolated = true;
    }
}
