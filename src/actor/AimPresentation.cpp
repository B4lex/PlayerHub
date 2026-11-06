#include "actor/AimPresentation.h"
#include "actor/ActorRegistry.h"
#include "actor/RemoteActor.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace AimPresentation
{
    namespace
    {
        // A graph's hkbCharacter is its BShkbAnimationGraph::characterInstance at 0xC0, and
        // the graph keeps its Actor at 0x210 (1.7.104 disassembly, as in AnimationFirewall).
        constexpr std::ptrdiff_t kGraphCharacterOffset = 0xC0;
        constexpr std::ptrdiff_t kGraphActorOffset = 0x210;

        // BSDirectAtModifierInterface vfunc 1 (ID 43657, SE 42496) gives the vanilla
        // BSDirectAtModifier its target, which bends the
        // spine for a bow draw or an aimed spell. Mode 1 asks for a head-tracking target,
        // 2 and 3 for an aim target. For the player it answers its position plus 4000
        // units along Euler (data.angle.x, 0, heading); for other actors, the aim target
        // their movement controller holds from combat (ID 37838), which a remote actor
        // never has, so it would aim level.
        constexpr std::int32_t kFirstAimMode = 2;
        constexpr std::int32_t kLastAimMode = 3;
        constexpr float kAimDistance = 4000.0F;

        const RE::Actor* GetGraphActor(const RE::hkbContext& a_context)
        {
            if (!a_context.character)
            {
                return nullptr;
            }
            const auto* graph = reinterpret_cast<const std::byte*>(a_context.character) - kGraphCharacterOffset;
            const RE::Actor* actor = nullptr;
            std::memcpy(&actor, graph + kGraphActorOffset, sizeof(actor));
            return actor;
        }

        struct DirectAtTargetHook
        {
            static void Thunk(void* a_interface, const RE::hkbContext& a_context, bool* a_found,
                RE::hkVector4* a_target, std::int32_t a_mode)
            {
                if (a_mode >= kFirstAimMode && a_mode <= kLastAimMode && a_found && a_target)
                {
                    const auto* actor = GetGraphActor(a_context);
                    if (const auto pitch = ActorRegistry::GetLookPitch(actor))
                    {
                        const auto position = actor->GetPosition();
                        const float heading = actor->GetAngleZ();
                        const float horizontal = std::cos(*pitch) * kAimDistance;
                        *a_target = RE::hkVector4{ position.x + std::sin(heading) * horizontal,
                            position.y + std::cos(heading) * horizontal, position.z - std::sin(*pitch) * kAimDistance,
                            0.0F };
                        *a_found = true;
                        return;
                    }
                }
                _original(a_interface, a_context, a_found, a_target, a_mode);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };
    }

    void InstallHooks()
    {
        REL::Relocation<std::uintptr_t> directAtVTable{ RE::VTABLE_BSDirectAtModifierInterface[0] };
        DirectAtTargetHook::_original = directAtVTable.write_vfunc(0x1, DirectAtTargetHook::Thunk);
        logs::debug("Installed remote actor aim target hook");
    }

    void SetLookPitch(RemoteActor& a_remote, float a_pitch)
    {
        ActorRegistry::SetLookPitch(a_remote, std::isfinite(a_pitch) ? a_pitch : 0.0F);
    }
}
