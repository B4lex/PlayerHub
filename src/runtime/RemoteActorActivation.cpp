#include "runtime/RemoteActorActivation.h"
#include "actor/ActorRegistry.h"
#include "sync/SyncCoordinator.h"

namespace RemoteActorActivation
{
    namespace
    {
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
                if (ActorRegistry::IsRegistered(actor))
                {
                    SyncCoordinator::QueueLocalActorActivation(actor->GetFormID());
                }
                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            ActivateEventSink() = default;
        };
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
