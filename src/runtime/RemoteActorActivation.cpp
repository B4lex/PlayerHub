#include "runtime/RemoteActorActivation.h"
#include "actor/ActorRegistry.h"
#include "sync/SyncCoordinator.h"

#include <cstdint>

namespace RemoteActorActivation
{
    namespace
    {
        struct ActivateTextHook
        {
            // The HUD's crosshair update (ID 40621, SE 39535) shows no prompt when this returns false.
            static bool Thunk(RE::TESNPC* a_this, RE::TESObjectREFR* a_targetRef, RE::BSString& a_text, bool a_arg4)
            {
                if (a_targetRef && ActorRegistry::IsDead(a_targetRef->As<RE::Actor>()))
                {
                    return false;
                }
                return _original(a_this, a_targetRef, a_text, a_arg4);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };

        // Skyrim activates on a worker thread.
        class ActivateEventSink final : public RE::BSTEventSink<RE::TESActivateEvent>
        {
        public:
            static ActivateEventSink* GetSingleton()
            {
                static ActivateEventSink singleton;
                return std::addressof(singleton);
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESActivateEvent* a_event, RE::BSTEventSource<RE::TESActivateEvent>*) override
            {
                if (!a_event || !a_event->objectActivated
                    || a_event->actionRef.get() != RE::PlayerCharacter::GetSingleton())
                {
                    return RE::BSEventNotifyControl::kContinue;
                }
                const auto* actor = a_event->objectActivated->As<RE::Actor>();
                if (ActorRegistry::IsRegistered(actor) && !ActorRegistry::IsDead(actor))
                {
                    SyncCoordinator::QueueLocalActorActivation(actor->GetFormID());
                }
                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            ActivateEventSink() = default;
        };
    }

    void InstallHooks()
    {
        REL::Relocation<std::uintptr_t> npcVTable{ RE::VTABLE_TESNPC[0] };
        ActivateTextHook::_original = npcVTable.write_vfunc(0x4C, ActivateTextHook::Thunk);
        logs::debug("Installed remote actor activate text hook");
    }

    void RegisterEventSink()
    {
        static bool registered = false;
        if (registered)
        {
            return;
        }

        auto* eventSources = RE::ScriptEventSourceHolder::GetSingleton();
        if (!eventSources)
        {
            logs::critical("Unable to register the remote actor activation event sink");
            return;
        }

        eventSources->AddEventSink(ActivateEventSink::GetSingleton());
        registered = true;
        logs::debug("Registered remote actor activation event sink");
    }
}
