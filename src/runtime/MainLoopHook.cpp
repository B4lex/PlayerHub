#include "runtime/MainLoopHook.h"
#include "network/ConnectionMonitor.h"
#include "runtime/GameThread.h"
#include "runtime/PluginTime.h"
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
            }

            static void Update()
            {
                const auto delta = RE::Main::QFrameAnimTime();
                if (!std::isfinite(delta) || delta <= 0.0F || !GameThread::Bind())
                {
                    return;
                }

                SyncCoordinator::OnFrame(RE::PlayerCharacter::GetSingleton(), delta);
                Network::UpdateConnectionMonitor(delta);
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
