#pragma once

#include "audio/VoiceMixer.h"
#include "protocol/VoiceFrame.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Audio
{
    struct Settings
    {
        // Whether the devices are open: voice enabled with a connection that carries it.
        bool active{ false };
        // Device names as the system lists them; empty for the system's default device.
        std::string inputDevice;
        std::string outputDevice;
        float inputGain{ 1.0F };
        float outputGain{ 1.0F };
        bool noiseSuppression{ true };
        bool voiceActivation{ false };
        // On the input meter's scale, 0 to 1.
        float activationLevel{ 0.35F };

        bool operator==(const Settings&) const = default;
    };

    using FrameSink = void (*)(std::uint16_t a_sequence, bool a_end, std::span<const std::uint8_t> a_opus);

    // Any thread. Opening and closing devices happens on the engine's worker, so the caller never
    // waits for a driver.
    void Configure(const Settings& a_settings);

    void SetFrameSink(FrameSink a_sink);

    // a_allowed: the player may be heard now. The key only counts while the game's window is in front.
    void SetTransmission(bool a_allowed, bool a_pushToTalkHeld);

    // Network threads.
    void PushFrame(const Multiplayer::VoiceFrame& a_frame);

    // Game thread, once per pass.
    void SetSpeakers(std::span<const Speaker> a_speakers);
    void CopySpeakerLevels(std::vector<SpeakerLevel>& a_levels);

    [[nodiscard]] float GetInputLevel();
    [[nodiscard]] bool IsTransmitting();
    // Whether the active devices opened; false while inactive.
    [[nodiscard]] bool IsInputOpen();
    [[nodiscard]] bool IsOutputOpen();

    // The names last listed by the system; RefreshDevices lists them again on the worker.
    void CopyDeviceNames(bool a_capture, std::vector<std::string>& a_names);
    void RefreshDevices();
}
