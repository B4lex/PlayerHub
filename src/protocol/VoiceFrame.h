#pragma once

#include "protocol/PeerSlot.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace Multiplayer
{
    inline constexpr std::uint32_t kVoiceSampleRate = 48000;
    // 20 ms of mono audio, one Opus frame.
    inline constexpr std::size_t kVoiceFrameSamples = 960;
    inline constexpr std::size_t kMaximumVoiceFrameBytes = 400;
    inline constexpr std::size_t kVoiceKeyBytes = 32;
    inline constexpr std::uint32_t kMinimumVoiceRadius = 100;
    inline constexpr std::uint32_t kMaximumVoiceRadius = 65535;
    inline constexpr std::size_t kMaximumVoiceHostBytes = 253;

    // How a connection reaches the voice relay, from its Welcome.
    struct VoiceAccess
    {
        // Empty for the host the WebSocket connected to.
        std::string host;
        std::uint16_t port{ 0 };
        std::uint32_t id{ 0 };
        std::array<std::uint8_t, kVoiceKeyBytes> key{};
        std::uint32_t radius{ 0 };
    };

    // One Opus frame of a speaker. Fixed-size, so a frame crosses threads without allocating.
    struct VoiceFrame
    {
        PeerSlot speaker{ 0 };
        std::uint16_t sequence{ 0 };
        bool end{ false };
        std::uint16_t size{ 0 };
        std::array<std::uint8_t, kMaximumVoiceFrameBytes> opus{};

        [[nodiscard]] std::span<const std::uint8_t> GetOpus() const noexcept { return { opus.data(), size }; }
    };

    [[nodiscard]] constexpr bool IsValidVoiceFrameSize(std::size_t a_size) noexcept
    {
        return a_size > 0 && a_size <= kMaximumVoiceFrameBytes;
    }

    // A host name or address the relay may name: printable ASCII without spaces or a path.
    [[nodiscard]] constexpr bool IsValidVoiceHost(std::string_view a_host) noexcept
    {
        if (a_host.size() > kMaximumVoiceHostBytes)
        {
            return false;
        }
        for (const auto character : a_host)
        {
            if (character <= ' ' || character > '~' || character == '/' || character == '\\')
            {
                return false;
            }
        }
        return true;
    }
}
