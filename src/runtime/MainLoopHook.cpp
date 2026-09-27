#include "runtime/MainLoopHook.h"
#include "network/ConnectionMonitor.h"
#include "runtime/GameThread.h"
#include "runtime/PluginTime.h"
#include "runtime/Profiling.h"
#include "sync/SyncCoordinator.h"

#include <cmath>
#include <cstdint>

namespace MainLoopHook
{
    namespace
    {
        struct MainLoopUpdateHook
        {
            static void Thunk(std::int64_t a_context)
            {
                _original(a_context);

                {
                    const PluginTime::Scope timing;
                    Update();
                }
                PluginTime::EndFrame();
                PROFILE_FRAME();
            }

            static void Update()
            {
                PROFILE_SCOPE("MainLoopHook");
                if (!GameThread::Bind())
                {
                    return;
                }

                // Frame time stops in the main menu and while the game is paused.
                // The connection still follows the game and the player's choice
                // there, with its timers held.
                const auto delta = RE::Main::QFrameAnimTime();
                const bool advancing = std::isfinite(delta) && delta > 0.0F;
                if (advancing)
                {
                    SyncCoordinator::OnFrame(RE::PlayerCharacter::GetSingleton(), delta);
                }
                Network::UpdateConnectionMonitor(advancing ? delta : 0.0F);
            }

            static inline REL::Relocation<decltype(Thunk)> _original;
        };
    }

    void Install()
    {
        REL::Relocation<std::uintptr_t> mainLoopUpdateCall{ RELOCATION_ID(35565, 36564),
            REL::VariantOffset(0x53, 0x6E, 0x68) };
        MainLoopUpdateHook::_original
            = SKSE::GetTrampoline().write_call<5>(mainLoopUpdateCall.address(), MainLoopUpdateHook::Thunk);

        logs::debug("Installed authoritative Skyrim main-loop network frame hook");
    }
}
