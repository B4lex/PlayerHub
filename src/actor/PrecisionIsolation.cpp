#include "actor/PrecisionIsolation.h"
#include "runtime/FrameTime.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace PrecisionIsolation
{
    namespace
    {
        // PRECISION_API::InterfaceVersion::V3 (PrecisionAPI.h), the first with
        // ToggleDisableActor. Every version returns the same IVPrecision4 object.
        constexpr std::uint8_t kInterfaceVersion3 = 2;
        // IVPrecision3::ToggleDisableActor follows IVPrecision1's nine and
        // IVPrecision2's five virtual functions and IVPrecision3's first nine; the
        // interfaces have no virtual destructor.
        constexpr std::size_t kToggleDisableActorSlot = 23;
        // Precision clears its disabled actors on its first pass after the
        // player's Havok world changes, then waits fWorldChangedWaitTime (0.4 s by
        // default) before adding actors to its world again.
        constexpr float kWorldChangeReassertSeconds = 1.0F;

        using RequestPluginApi = void* (*)(std::uint8_t);
        // x64 passes `this` as the first argument, so the member function can be
        // called through its vtable slot.
        using ToggleDisableActor = bool (*)(void*, RE::ActorHandle, bool);

        void* s_precision{ nullptr };
        ToggleDisableActor s_toggleDisableActor{ nullptr };
        std::vector<RE::ActorHandle> s_excluded;
        // Compared for identity only; never dereferenced.
        const RE::bhkWorld* s_playerWorld{ nullptr };
        float s_reassertRemaining{ 0.0F };

        void SetDisabled(RE::ActorHandle a_handle, bool a_disabled)
        {
            s_toggleDisableActor(s_precision, a_handle, a_disabled);
        }
    }

    void Initialize()
    {
        const auto module = REX::W32::GetModuleHandleW(L"Precision.dll");
        if (!module)
        {
            logs::debug("Precision is not loaded");
            return;
        }

        const auto request = reinterpret_cast<RequestPluginApi>(REX::W32::GetProcAddress(module, "RequestPluginAPI"));
        s_precision = request ? request(kInterfaceVersion3) : nullptr;
        if (!s_precision)
        {
            logs::warn("Precision is loaded without interface V3; remote actors keep its attack collisions, and only "
                       "the actor hit hook drops their hits");
            return;
        }

        s_toggleDisableActor
            = reinterpret_cast<ToggleDisableActor>((*static_cast<void***>(s_precision))[kToggleDisableActorSlot]);
        logs::info("Precision interface V3 found; remote actors are excluded from Precision");
    }

    void Exclude(RE::ActorHandle a_handle)
    {
        if (!s_toggleDisableActor)
        {
            return;
        }
        s_excluded.push_back(a_handle);
        SetDisabled(a_handle, true);
    }

    void Release(RE::ActorHandle a_handle)
    {
        if (!s_toggleDisableActor)
        {
            return;
        }
        if (std::erase(s_excluded, a_handle) > 0)
        {
            SetDisabled(a_handle, false);
        }
    }

    void Update(const RE::TESObjectCELL& a_playerCell, float a_deltaTime)
    {
        if (!s_toggleDisableActor)
        {
            return;
        }

        const auto* world = a_playerCell.GetbhkWorld();
        if (world != s_playerWorld)
        {
            s_playerWorld = world;
            s_reassertRemaining = kWorldChangeReassertSeconds;
            logs::debug("Player Havok world changed; reasserting Precision exclusion for {} remote actors",
                s_excluded.size());
        }
        if (s_reassertRemaining <= 0.0F)
        {
            return;
        }

        s_reassertRemaining -= FrameTime::Clamp(a_deltaTime);
        for (const auto& handle : s_excluded)
        {
            SetDisabled(handle, true);
        }
    }
}
