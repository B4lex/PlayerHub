#include "sync/presence/PresenceSync.h"
#include "network/SocketIOClient.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>

namespace PresenceSync
{
    namespace
    {
        // Exits are also reported by the menu sink and SKSE messages, outside the
        // main-loop stage.
        std::mutex s_lock;
        // Whether the server was told the player is in the world on
        // s_publishedConnectionGeneration.
        bool s_published{ false };
        std::uint64_t s_publishedConnectionGeneration{ 0 };

        // Requires s_lock.
        void PublishExitLocked(std::string_view a_reason)
        {
            if (!s_published)
            {
                return;
            }
            s_published = false;
            Network::PublishLocalWorldExit();
            logs::info("Local player left the world: reason={} connectionGeneration={}", a_reason,
                static_cast<unsigned long long>(s_publishedConnectionGeneration));
        }

        bool IsWorldHidingMenu(std::string_view a_menuName)
        {
            return a_menuName == RE::LoadingMenu::MENU_NAME || a_menuName == RE::MainMenu::MENU_NAME;
        }

        class MenuEventSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
        public:
            static MenuEventSink* GetSingleton()
            {
                static MenuEventSink singleton;
                return std::addressof(singleton);
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
            {
                if (a_event && a_event->opening && IsWorldHidingMenu(a_event->menuName.c_str()))
                {
                    std::scoped_lock lock{ s_lock };
                    PublishExitLocked(a_event->menuName.c_str());
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        bool IsInWorld(const LocalFrame& a_local)
        {
            auto* ui = RE::UI::GetSingleton();
            if (!ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || ui->IsMenuOpen(RE::MainMenu::MENU_NAME))
            {
                return false;
            }
            return a_local.player.GetParentCell() && a_local.player.Is3DLoaded();
        }
    }

    void RegisterEventSinks()
    {
        static bool registered = false;
        if (registered)
        {
            return;
        }

        auto* ui = RE::UI::GetSingleton();
        if (!ui)
        {
            logs::warn("Could not register the world presence menu sink: UI is unavailable");
            return;
        }
        ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuEventSink::GetSingleton());
        registered = true;
        logs::info("Registered world presence menu sink");
    }

    void OnGameLoadStarted()
    {
        std::scoped_lock lock{ s_lock };
        PublishExitLocked("save load");
    }

    bool SyncPresence(const LocalFrame& a_local)
    {
        const bool inWorld = IsInWorld(a_local);

        std::scoped_lock lock{ s_lock };
        if (!Network::IsConnected())
        {
            return inWorld;
        }
        if (const auto generation = Network::GetConnectionGeneration(); generation != s_publishedConnectionGeneration)
        {
            // Every connection starts out of the world on the server.
            s_publishedConnectionGeneration = generation;
            s_published = false;
        }

        if (inWorld && !s_published)
        {
            s_published = true;
            Network::PublishLocalWorldEnter();
            logs::info("Local player entered the world: connectionGeneration={}",
                static_cast<unsigned long long>(s_publishedConnectionGeneration));
        }
        else if (!inWorld)
        {
            PublishExitLocked("main-loop check");
        }
        return inWorld;
    }
}
