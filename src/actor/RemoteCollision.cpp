#include "actor/RemoteCollision.h"
#include "actor/ActorRegistry.h"

#include <cstdint>
#include <memory>

namespace RemoteCollision
{
    namespace
    {
        // A moving character is blocked by the other character's rigid body its
        // controller bumped into. As in powerofthree's Tweaks' TCL fix, that body
        // ignores collision only for the mover's movement update, so both
        // controllers keep colliding with the world.
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
    }

    void InstallHooks()
    {
        // The call site is verified against 1.7.104 only.
        if (!REL::Module::IsAE())
        {
            logs::warn(
                "Remote actor pass-through collision is verified only on Skyrim AE; remote actors may "
                "body-block characters");
            return;
        }

        // Actor movement update call (ID 37350 + 0xFB on 1.7.104), as patched by
        // powerofthree's Tweaks. write_call chains onto another plugin's detour.
        constexpr std::ptrdiff_t kUpdateMovementCallOffset = 0xFB;
        const auto site = REL::Relocation<std::uintptr_t>{ REL::ID(37350) }.address() + kUpdateMovementCallOffset;
        if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8)
        {
            logs::error("Did not install remote actor pass-through collision hook: no call at ID 37350 + {:#x}",
                kUpdateMovementCallOffset);
            return;
        }
        UpdateMovementHook::_original
            = SKSE::GetTrampoline().write_call<5>(site, reinterpret_cast<std::uintptr_t>(&UpdateMovementHook::Thunk));
        logs::info("Installed remote actor pass-through collision hook");
    }

    bool SetCharacterBodyCollision(RE::Actor& a_actor, bool a_enabled)
    {
        // The body other characters bump and the hook above disables. A proxy
        // controller keeps its world collision in a separate phantom.
        auto* controller = a_actor.GetCharController();
        auto* body = controller ? controller->GetRigidBody() : nullptr;
        if (!body)
        {
            return false;
        }
        auto& filter = body->collidable.broadPhaseHandle.collisionFilterInfo;
        if (filter.QNoCollision() != a_enabled)
        {
            return false;
        }
        filter.SetNoCollision(!a_enabled);
        return true;
    }
}
