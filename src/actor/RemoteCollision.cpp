#include "actor/RemoteCollision.h"
#include "actor/ActorRegistry.h"
#include "actor/GameplayPolicy.h"
#include "actor/RemoteActor.h"
#include "runtime/CodeDetour.h"
#include "runtime/FrameTime.h"
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

        // Every caller of the collision filter's info comparison (ID 78553, SE 76681)
        // on 1.5.97 and on 1.7.104, found by scanning both executables: the pair,
        // shape, ray, and phantom filters.
        constexpr std::array kFilterInfoCallers{
            RELOCATION_ID(76181, 78009),
            RELOCATION_ID(76676, 78548),
            RELOCATION_ID(76677, 78549),
            RELOCATION_ID(76678, 78550),
            RELOCATION_ID(76679, 78551),
            RELOCATION_ID(76680, 78552),
            RELOCATION_ID(77228, 79115),
        };
        static_assert(kFilterInfoCallers.size() == kFilterInfoCallerCount);

        constexpr std::uint32_t kLayerMask = RE::CFilter::Flags::kLayerMask;
        constexpr std::uint32_t kCharControllerLayer = std::to_underlying(RE::COL_LAYER::kCharController);
        // The comparison lets a first side on this layer past the no-collision bit.
        constexpr std::uint32_t kNoCollisionExemptLayer = std::to_underlying(RE::COL_LAYER::kCustomPick2);

        // Written once by InstallHooks on the main thread before the game loop starts.
        bool s_solidFilterInstalled{ false };
        // The local player's controller phantom's filter info, which the comparison
        // receives for the player's movement; 0 while the player has none.
        std::atomic<std::uint32_t> s_playerControllerInfo{ 0 };

        std::uint16_t GetSystemGroup(std::uint32_t a_info)
        {
            return static_cast<std::uint16_t>(a_info >> 16);
        }

        bool IsSolidCharacterBody(std::uint32_t a_info)
        {
            return (a_info & kLayerMask) == kCharControllerLayer
                && ActorRegistry::IsSolidCharacterGroup(GetSystemGroup(a_info));
        }

        // A solid remote character body collides with the local player's controller
        // alone. Pairs within one actor's system group stay the comparison's own.
        bool MaySolidPairCollide(std::uint32_t a_infoA, std::uint32_t a_infoB)
        {
            const bool solidA = IsSolidCharacterBody(a_infoA);
            const bool solidB = IsSolidCharacterBody(a_infoB);
            if ((!solidA && !solidB) || GetSystemGroup(a_infoA) == GetSystemGroup(a_infoB))
            {
                return true;
            }
            if (solidA && solidB)
            {
                return false;
            }
            if (solidB && (a_infoA & kLayerMask) == kNoCollisionExemptLayer)
            {
                return true;
            }
            const auto other = solidA ? a_infoB : a_infoA;
            const auto player = s_playerControllerInfo.load(std::memory_order_relaxed);
            return player != 0 && (other & kLayerMask) == kCharControllerLayer
                && GetSystemGroup(other) == GetSystemGroup(player);
        }

        // Takes two filter infos and returns whether they collide. The pair filter
        // behind broadphase pairs, phantom overlaps, and contact agents, and the ray
        // and shape filters behind queries, all call it.
        struct FilterInfoHook
        {
            static bool Thunk(void* a_filter, std::uint32_t a_infoA, std::uint32_t a_infoB)
            {
                if (ActorRegistry::HasSolidCharacters() && !MaySolidPairCollide(a_infoA, a_infoB))
                {
                    return false;
                }
                return _original(a_filter, a_infoA, a_infoB);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // A network-placed character body is solid to the local player while
        // [Gameplay] DisableCollision is off and every comparison call is detoured.
        bool IsSolidToPlayer()
        {
            return s_solidFilterInstalled && !GameplayPolicy::DisablesCollision();
        }

        // Skyrim's movement update (ID 37356) moves an actor by its animation's
        // root motion through its character controller, which pushes out of and
        // pushes local bodies, and then feeds the achieved motion back into the
        // graph. For a network-placed actor all of that fights the network
        // transform, so it is skipped. Otherwise, the character body a moving
        // controller bumped into ignores collision for that update when either
        // character is remote.
        //
        // The player's proxy controller also collides with remote skeleton
        // bodies, such as a seated rider's legs at its mount's flanks. Its
        // phantom's overlap list holds every body its movement can touch, so the
        // remote character and skeleton bodies in it ignore collision for the
        // player's update, and keep it for everything else.
        struct UpdateMovementHook
        {
            static void AddBumpedCharacter(RE::Actor* a_mover, PassThrough& a_passThrough, bool a_solidToPlayer)
            {
                if (a_solidToPlayer && a_mover && a_mover->IsPlayerRef())
                {
                    return;
                }
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

            static void AddRemoteBody(RE::hkpCollidable* a_collidable, PassThrough& a_passThrough, bool a_solidToPlayer)
            {
                if (!a_collidable)
                {
                    return;
                }
                auto& filter = a_collidable->broadPhaseHandle.collisionFilterInfo;
                const auto layer = filter.GetCollisionLayer();
                if (filter.QNoCollision() || !IsCharacterLayer(layer)
                    || (a_solidToPlayer && layer == RE::COL_LAYER::kCharController))
                {
                    return;
                }
                auto* ref = RE::TESHavokUtilities::FindCollidableRef(*a_collidable);
                if (ref && ActorRegistry::IsRegistered(ref->As<RE::Actor>()))
                {
                    a_passThrough.Add(filter);
                }
            }

            static void AddOverlappingRemoteBodies(
                RE::Actor& a_player, PassThrough& a_passThrough, bool a_solidToPlayer)
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
                            AddRemoteBody(collidable, a_passThrough, a_solidToPlayer);
                        }
                        break;
                    case RE::hkpPhantomType::kCachingShape:
                        for (auto& detail : static_cast<RE::hkpCachingShapePhantom*>(phantom)->collisionDetails)
                        {
                            AddRemoteBody(detail.collidable, a_passThrough, a_solidToPlayer);
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
                const bool solidToPlayer = IsSolidToPlayer();
                AddBumpedCharacter(a_actor, passThrough, solidToPlayer);
                if (a_actor && a_actor->IsPlayerRef())
                {
                    AddOverlappingRemoteBodies(*a_actor, passThrough, solidToPlayer);
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
        // triggers. It needs its collision only while Skyrim moves the actor, or with
        // the local player's controller alone while it is solid to the player.
        enum class BodyCollision
        {
            kIsolated,
            kSolidToPlayer,
            kRestored,
        };

        std::string_view GetName(BodyCollision a_collision)
        {
            switch (a_collision)
            {
            case BodyCollision::kIsolated:
                return "disabled";
            case BodyCollision::kSolidToPlayer:
                return "solid to the local player";
            case BodyCollision::kRestored:
            default:
                return "restored";
            }
        }

        void SetCharacterBodyCollision(
            RE::Actor& a_actor, RemoteActor& a_remote, BodyCollision a_collision, std::string_view a_owner)
        {
            auto* controller = a_actor.GetCharController();
            auto* body = controller ? controller->GetRigidBody() : nullptr;
            auto* filter = body ? std::addressof(body->collidable.broadPhaseHandle.collisionFilterInfo) : nullptr;
            const auto group = filter ? static_cast<std::uint16_t>(filter->GetSystemGroup()) : std::uint16_t{ 0 };
            if (a_collision == BodyCollision::kSolidToPlayer && group == 0)
            {
                // Without a system group the filter cannot tell the body apart.
                a_collision = BodyCollision::kIsolated;
            }
            const auto solidGroup = a_collision == BodyCollision::kSolidToPlayer ? group : std::uint16_t{ 0 };
            const bool groupChanged = a_remote.collision.solidGroup != solidGroup;
            // The group counts before the bit clears and stops after it is set, so the
            // body never collides with everything in between.
            if (solidGroup != 0)
            {
                ActorRegistry::SetSolidCharacterGroup(a_remote, solidGroup);
            }
            const bool isolated = a_collision == BodyCollision::kIsolated;
            const bool bitChanged = filter && filter->QNoCollision() != isolated;
            if (bitChanged)
            {
                filter->SetNoCollision(isolated);
            }
            if (solidGroup == 0)
            {
                ActorRegistry::SetSolidCharacterGroup(a_remote, 0);
            }
            a_remote.collision.solidGroup = solidGroup;
            if (bitChanged || groupChanged)
            {
                logs::debug("Remote actor character body collision {}: actor={:08X} owner={} controller={} group={}",
                    GetName(a_collision), a_actor.GetHandle().native_handle(), a_owner,
                    skyrim_cast<RE::bhkCharProxyController*>(controller) ? "proxy" : "rigidBody", group);
            }
        }

        constexpr float kSkeletonRecheckSeconds = 0.5F;

        // A live skeleton's bone and weapon bodies, and a dead one's ragdoll.
        bool IsSkeletonLayer(RE::COL_LAYER a_layer)
        {
            return a_layer == RE::COL_LAYER::kBiped || a_layer == RE::COL_LAYER::kBipedNoCC
                || a_layer == RE::COL_LAYER::kDeadBip;
        }

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
        std::size_t installed = 0;
        for (const auto& caller : kFilterInfoCallers)
        {
            if (const auto original = CodeDetour::WriteCalleeDetour(REL::ID(caller),
                    REL::ID(RELOCATION_ID(76681, 78553)), reinterpret_cast<std::uintptr_t>(&FilterInfoHook::Thunk),
                    "collision filter info"))
            {
                FilterInfoHook::_original = original;
                ++installed;
            }
        }
        s_solidFilterInstalled = installed == kFilterInfoCallers.size();
        logs::debug("Remote actor collision filter hooks: installed={}/{}", installed, kFilterInfoCallers.size());
        if (!s_solidFilterInstalled)
        {
            logs::error("Remote actor collision filter is incomplete: remote players stay passable");
        }

        // Actor movement update call (ID 37350 + 0xFB on 1.7.104, ID 36359 + 0xF0 on
        // SE), its only caller, as patched by powerofthree's Tweaks. write_call chains
        // onto another plugin's detour.
        const REL::RelocationID callerID{ 36359, 37350 };
        const auto callOffset = REL::Relocate(0xF0, 0xFB);
        const auto site = REL::Relocation<std::uintptr_t>{ callerID }.address() + callOffset;
        if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8)
        {
            logs::error("Did not install remote actor movement isolation hook: no call at ID {} + {:#x}", callerID.id(),
                callOffset);
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
        SetCharacterBodyCollision(a_actor, a_remote,
            IsSolidToPlayer() ? BodyCollision::kSolidToPlayer : BodyCollision::kIsolated, "network");
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
        SetCharacterBodyCollision(
            a_actor, a_remote, a_carried ? BodyCollision::kIsolated : BodyCollision::kRestored, a_owner);
        if (!a_carried && a_remote.collision.skeletonIsolated)
        {
            a_remote.collision.skeletonIsolated = false;
            const auto changed = SetSkeletonIsolated(a_actor, false);
            logs::debug("Remote actor skeleton collision restored: actor={:08X} owner={} bodies={}",
                a_remote.handle.native_handle(), a_owner, changed);
        }
    }

    void TrackLocalPlayer(RE::PlayerCharacter& a_player)
    {
        auto* controller = skyrim_cast<RE::bhkCharProxyController*>(a_player.GetCharController());
        auto* proxy = controller ? controller->GetCharacterProxy() : nullptr;
        auto* phantom = proxy ? proxy->shapePhantom : nullptr;
        s_playerControllerInfo.store(
            phantom ? phantom->collidable.broadPhaseHandle.collisionFilterInfo.filter : 0, std::memory_order_relaxed);
    }

    void IsolateCarriedSkeleton(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime)
    {
        auto& collision = a_remote.collision;
        if (collision.skeletonIsolated)
        {
            collision.skeletonRecheckElapsed += FrameTime::Clamp(a_deltaTime);
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
