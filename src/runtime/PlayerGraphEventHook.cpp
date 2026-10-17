#include "runtime/PlayerGraphEventHook.h"
#include "sync/shapeshift/ShapeshiftSync.h"

#include <cstdint>

namespace PlayerGraphEventHook
{
    namespace
    {
        // Actions, idles, casts, and scripts reach the player's graph through this
        // call, on whichever thread raises them.
        struct GraphInputHook
        {
            static bool Thunk(RE::IAnimationGraphManagerHolder* a_holder, const RE::BSFixedString& a_eventName)
            {
                const bool accepted = _original(a_holder, a_eventName);
                if (accepted)
                {
                    ShapeshiftSync::ObserveLocalGraphEvent(a_eventName);
                }
                return accepted;
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };
    }

    void Install()
    {
        // As for Character (AnimationFirewall), index 3 is the
        // IAnimationGraphManagerHolder subobject, whose vfunc 1 is NotifyAnimationGraph.
        REL::Relocation<std::uintptr_t> playerAnimationGraphVTable{ RE::VTABLE_PlayerCharacter[3] };
        GraphInputHook::_original = playerAnimationGraphVTable.write_vfunc(0x1, GraphInputHook::Thunk);
        logs::debug("Installed local player graph input hook");
    }
}
