#include "runtime/FurnitureActivationHook.h"
#include "actor/ActorRegistry.h"
#include "runtime/GameThread.h"
#include "runtime/Profiling.h"
#include "sync/SyncCoordinator.h"

#include <cstdint>

namespace FurnitureActivationHook
{
    namespace
    {
        PROFILE_HOOK_COUNTER(s_furnitureActivationProfile, "furniture activation");

        struct ActivateHook
        {
            static bool Thunk(RE::TESFurniture* a_this, RE::TESObjectREFR* a_targetRef,
                RE::TESObjectREFR* a_activatorRef, std::uint8_t a_arg3, RE::TESBoundObject* a_object,
                std::int32_t a_targetCount)
            {
                PROFILE_HOOK(s_furnitureActivationProfile);
                if (!a_targetRef || !a_activatorRef || a_activatorRef != RE::PlayerCharacter::GetSingleton())
                {
                    return PROFILE_HOOK_ORIGINAL(
                        _original(a_this, a_targetRef, a_activatorRef, a_arg3, a_object, a_targetCount));
                }

                // Skyrim activates on a worker thread. Furniture a remote actor holds
                // would refuse the player with an "in use" message.
                const auto furnitureRefID = a_targetRef->GetFormID();
                const bool deferred = !GameThread::IsAuthoritative() && ActorRegistry::IsFurnitureHeld(furnitureRefID);
                SyncCoordinator::QueueLocalFurnitureActivation(furnitureRefID, deferred);
                return deferred
                    || PROFILE_HOOK_ORIGINAL(
                        _original(a_this, a_targetRef, a_activatorRef, a_arg3, a_object, a_targetCount));
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };
    }

    void Install()
    {
        REL::Relocation<std::uintptr_t> furnitureVTable{ RE::VTABLE_TESFurniture[0] };
        ActivateHook::_original = furnitureVTable.write_vfunc(0x37, ActivateHook::Thunk);
        logs::debug("Installed local furniture activation hook");
    }
}
