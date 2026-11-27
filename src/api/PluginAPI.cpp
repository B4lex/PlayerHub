#include <windows.h>

#include "PlayerHubAPI.h"
#include "sync/skeleton/PoseWindow.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

namespace
{
    constexpr std::size_t kMaximumOwner = 64;

    // Reads at most kMaximumOwner bytes, since a caller's name may lack a terminator.
    std::string ReadOwner(const char* a_owner)
    {
        std::string owner;
        for (std::size_t index = 0; a_owner && index < kMaximumOwner && a_owner[index] != '\0'; ++index)
        {
            const char character = a_owner[index];
            owner.push_back(character >= ' ' && character <= '~' ? character : '?');
        }
        if (owner.empty())
        {
            owner = "unnamed";
        }
        return owner;
    }

    class PlayerHubInterface final : public PlayerHubAPI::IVPlayerHub1
    {
    public:
        PlayerHubAPI::PoseWindowHold OpenPoseWindow(const char* a_owner) noexcept override
        {
            try
            {
                return PoseWindow::AcquireHold(ReadOwner(a_owner));
            }
            catch (const std::exception& a_error)
            {
                logs::error("Plugin API could not open a pose window: {}", a_error.what());
                return 0;
            }
        }

        void ClosePoseWindow(PlayerHubAPI::PoseWindowHold a_hold) noexcept override
        {
            try
            {
                PoseWindow::ReleaseHold(a_hold);
            }
            catch (const std::exception& a_error)
            {
                logs::error("Plugin API could not close a pose window: {}", a_error.what());
            }
        }
    };

    PlayerHubInterface s_interface;
}

extern "C" __declspec(dllexport) void* RequestPluginAPI(std::uint32_t a_version)
{
    if (a_version != PlayerHubAPI::kInterfaceVersion1)
    {
        logs::warn("Plugin API refused an unknown interface version: version={}", a_version);
        return nullptr;
    }
    logs::info("Plugin API interface requested: version={}", a_version);
    return static_cast<PlayerHubAPI::IVPlayerHub1*>(&s_interface);
}
