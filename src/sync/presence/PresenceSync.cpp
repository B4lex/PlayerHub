#include "sync/presence/PresenceSync.h"
#include "network/ServerClient.h"
#include "runtime/GameSession.h"
#include "runtime/Profiling.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>

namespace PresenceSync
{
    namespace
    {
        std::mutex s_lock;
        bool s_published{ false };
        std::uint64_t s_publishedConnectionGeneration{ 0 };

        void PublishExitLocked(std::string_view a_reason)
        {
            if (!s_published)
            {
                return;
            }
            s_published = false;
            Network::PublishLocalWorldExit();
            logs::debug("Local player left the world: reason={} connectionGeneration={}", a_reason,
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
                if (!a_event || !a_event->opening)
                {
                    return RE::BSEventNotifyControl::kContinue;
                }
                const std::string_view menuName = a_event->menuName.c_str();
                if (IsWorldHidingMenu(menuName))
                {
                    std::scoped_lock lock{ s_lock };
                    PublishExitLocked(menuName);
                }
                if (menuName == RE::MainMenu::MENU_NAME)
                {
                    GameSession::SetInGame(false);
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
        logs::debug("Registered world presence menu sink");
    }

    void OnGameLoadStarted()
    {
        std::scoped_lock lock{ s_lock };
        PublishExitLocked("save load");
    }

    bool SyncPresence(const LocalFrame& a_local)
    {
        PROFILE_FUNCTION();
        const bool inWorld = IsInWorld(a_local);
        if (inWorld)
        {
            GameSession::SetInGame(true);
        }

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
            logs::debug("Local player entered the world: connectionGeneration={}",
                static_cast<unsigned long long>(s_publishedConnectionGeneration));
        }
        else if (!inWorld)
        {
            PublishExitLocked("main-loop check");
        }
        return inWorld;
    }
}
