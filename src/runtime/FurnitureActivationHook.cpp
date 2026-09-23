#include "runtime/FurnitureActivationHook.h"
#include "actor/ActorRegistry.h"
#include "runtime/GameThread.h"
#include "sync/SyncCoordinator.h"

#include <cstdint>

namespace FurnitureActivationHook
{
    namespace
    {
        struct ActivateHook
        {
            static bool Thunk(RE::TESFurniture* a_this, RE::TESObjectREFR* a_targetRef,
                RE::TESObjectREFR* a_activatorRef, std::uint8_t a_arg3, RE::TESBoundObject* a_object,
                std::int32_t a_targetCount)
            {
                if (!a_targetRef || !a_activatorRef || a_activatorRef != RE::PlayerCharacter::GetSingleton())
                {
                    return _original(a_this, a_targetRef, a_activatorRef, a_arg3, a_object, a_targetCount);
                }

                // Skyrim activates on a worker thread, where only the reference's
                // identity is read. Furniture a remote actor holds would refuse the
                // player with an "in use" message, so that activation is left to
                // the main loop, which repeats it once the actor has yielded.
                const auto furnitureRefID = a_targetRef->GetFormID();
                const bool deferred = !GameThread::IsAuthoritative() && ActorRegistry::IsFurnitureHeld(furnitureRefID);
                SyncCoordinator::QueueLocalFurnitureActivation(furnitureRefID, deferred);
                return deferred || _original(a_this, a_targetRef, a_activatorRef, a_arg3, a_object, a_targetCount);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };
    }

    void Install()
    {
        REL::Relocation<std::uintptr_t> furnitureVTable{ RE::VTABLE_TESFurniture[0] };
        ActivateHook::_original = furnitureVTable.write_vfunc(0x37, ActivateHook::Thunk);
        logs::info("Installed local furniture activation hook");
    }
}
