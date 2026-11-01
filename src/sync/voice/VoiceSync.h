#pragma once

#include "network/VoiceTransport.h"
#include "protocol/PeerSlot.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct PeerState;

namespace VoiceSync
{
    struct FilterRevision
    {
        std::uint64_t roster{ 0 };
        std::uint64_t filters{ 0 };

        bool operator==(const FilterRevision&) const = default;
    };

    struct RemoteState
    {
        std::optional<FilterRevision> filterRevision;
        bool muted{ false };
        float volume{ 1.0F };
        bool occluded{ false };
        float occlusionCheckRemaining{ 0.0F };
        bool mouthOpen{ false };
    };

    struct LocalStatus
    {
        // The connection carries voice and voice is enabled.
        bool active{ false };
        bool microphoneOpen{ false };
        bool transmitting{ false };
        bool voiceActivation{ false };
        // Voice activation only: the microphone key turned the microphone off.
        bool microphoneOff{ false };
        float inputLevel{ 0.0F };
        Network::VoiceTransport::Route route{ Network::VoiceTransport::Route::kNone };
    };

    // At plugin load: joins the audio engine and the voice transport.
    void Initialize();

    // Every main-loop frame, also while paused or in the main menu: devices follow the settings and
    // the connection, and the microphone key's state reaches the capture thread.
    void Maintain();

    // Each pass with a player.
    void UpdateLocal(bool a_inWorld);

    // Each pass, after every peer is synced: where each speaker is heard, occlusion, lip sync.
    void UpdatePeers(std::unordered_map<Multiplayer::PeerSlot, PeerState>& a_peers, RE::PlayerCharacter& a_player,
        RE::TESObjectCELL& a_cell, float a_delta);

    // The pass's playing level of a peer's voice, 0 while silent. Main-loop thread, like the overlay.
    [[nodiscard]] float GetSpeakingLevel(Multiplayer::PeerSlot a_playerID);

    [[nodiscard]] LocalStatus GetLocalStatus();

    void CopyDeviceNames(bool a_capture, std::vector<std::string>& a_names);
    void RefreshDevices();
}
