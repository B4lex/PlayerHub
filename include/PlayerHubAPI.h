#pragma once

#include <cstdint>

namespace PlayerHubAPI
{
    inline constexpr std::uint32_t kInterfaceVersion1 = 1;

    // 0 is never a hold.
    using PoseWindowHold = std::uint32_t;

    // Call from any thread, at or after SKSE's kPostLoad message.
    class IVPlayerHub1
    {
    public:
        // Streams the local player's skeleton pose to other players until the hold is
        // closed or a game loads, while PlayerHub's own conditions allow a window. Returns 0
        // when PlayerHub holds too many windows. The owner names the caller in PlayerHub's log.
        [[nodiscard]] virtual PoseWindowHold OpenPoseWindow(const char* a_owner) noexcept = 0;
        // Releases one hold; the window stays open while another hold or PlayerHub's own
        // detection keeps it. Unknown and already closed holds are ignored.
        virtual void ClosePoseWindow(PoseWindowHold a_hold) noexcept = 0;

    protected:
        ~IVPlayerHub1() = default;
    };

    // PlayerHub.dll exports it as "RequestPluginAPI"; it returns nullptr for an unknown version.
    using RequestPluginAPIFunction = void* (*)(std::uint32_t a_version);

    // Needs <windows.h>. Returns nullptr when PlayerHub is not loaded.
    [[nodiscard]] inline IVPlayerHub1* RequestPluginAPI()
    {
        const auto module = ::GetModuleHandleW(L"PlayerHub.dll");
        const auto request
            = module ? reinterpret_cast<RequestPluginAPIFunction>(::GetProcAddress(module, "RequestPluginAPI")) : nullptr;
        return request ? static_cast<IVPlayerHub1*>(request(kInterfaceVersion1)) : nullptr;
    }
}
