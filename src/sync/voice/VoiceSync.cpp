#include "sync/voice/VoiceSync.h"
#include "actor/ActorLipSync.h"
#include "actor/VoiceOcclusion.h"
#include "audio/AudioEngine.h"
#include "config/PluginConfig.h"
#include "runtime/FrameTime.h"
#include "runtime/GameSession.h"
#include "runtime/GameThread.h"
#include "runtime/KeyState.h"
#include "runtime/Profiling.h"
#include "sync/PeerState.h"
#include "sync/chat/ChatRoster.h"
#include "sync/filters/PlayerFilters.h"

#include <algorithm>
#include <cmath>

namespace VoiceSync
{
    namespace
    {
        using Network::VoiceTransport::Route;

        // Full volume within about four metres, then an inverse-distance fall that is gentler than
        // the physical one, so a voice carries across a camp; the last fifth of the radius fades to
        // nothing, where the relay stops sending.
        constexpr float kFullVolumeDistance = 300.0F;
        constexpr float kRolloffExponent = 0.7F;
        constexpr float kFadeFraction = 0.2F;
        // Heights of the ray between the listener's and the speaker's heads above their origins.
        constexpr float kListenerHeadHeight = 120.0F;
        constexpr float kSpeakerHeadHeight = 110.0F;
        constexpr float kSpeakingOcclusionSeconds = 0.25F;
        constexpr float kSilentOcclusionSeconds = 1.0F;
        // A cost cap per main-loop pass, not a duration.
        constexpr std::uint32_t kMaximumRaysPerPass = 4;
        constexpr float kSpeakingLevel = 0.05F;

        Audio::Settings s_settings;
        bool s_configured = false;
        bool s_inWorld = false;
        bool s_microphoneOff = false;
        std::uint32_t s_microphoneKey = 0;
        std::uint32_t s_presses = 0;

        std::vector<Audio::Speaker> s_speakers;
        std::vector<Audio::SpeakerLevel> s_levels;

        float Attenuate(float a_distance, float a_radius)
        {
            if (a_distance >= a_radius)
            {
                return 0.0F;
            }
            const auto rolloff = a_distance <= kFullVolumeDistance
                                   ? 1.0F
                                   : std::pow(kFullVolumeDistance / a_distance, kRolloffExponent);
            const auto fade = std::clamp((a_radius - a_distance) / (a_radius * kFadeFraction), 0.0F, 1.0F);
            return rolloff * fade;
        }

        float FindLevel(Multiplayer::PeerSlot a_playerID)
        {
            const auto level = std::ranges::lower_bound(s_levels, a_playerID, {}, &Audio::SpeakerLevel::slot);
            return level != s_levels.end() && level->slot == a_playerID ? level->level : 0.0F;
        }

        void RefreshFilter(RemoteState& a_state, Multiplayer::PeerSlot a_playerID, const FilterRevision& a_revision)
        {
            if (a_state.filterRevision == a_revision)
            {
                return;
            }
            a_state.filterRevision = a_revision;
            const auto player = ChatRoster::Find(a_playerID);
            a_state.muted = player && PlayerFilters::IsMuted(player->session);
            a_state.volume = player ? static_cast<float>(PlayerFilters::GetVoiceVolume(player->session)) / 100.0F : 1.0F;
        }

        void CloseMouth(RemoteState& a_state, RE::Actor* a_actor)
        {
            if (a_state.mouthOpen && a_actor)
            {
                ActorLipSync::Apply(*a_actor, 0.0F);
            }
            a_state.mouthOpen = false;
        }
    }

    void Initialize()
    {
        Audio::SetFrameSink([](std::uint16_t a_sequence, bool a_end, std::span<const std::uint8_t> a_opus) {
            Network::VoiceTransport::Send(a_sequence, a_end, a_opus);
        });
        Network::VoiceTransport::SetSink([](const Multiplayer::VoiceFrame& a_frame) { Audio::PushFrame(a_frame); });
    }

    void Maintain()
    {
        const auto config = Config::Get();
        const auto& voice = config->voice;
        const auto route = Network::VoiceTransport::GetRoute();
        const Audio::Settings settings{ .active = voice.enabled && route != Route::kNone && GameSession::IsInGame(),
            .inputDevice = voice.inputDevice,
            .outputDevice = voice.outputDevice,
            .inputGain = static_cast<float>(voice.inputVolume) / 100.0F,
            .outputGain = static_cast<float>(voice.outputVolume) / 100.0F,
            .noiseSuppression = voice.noiseSuppression,
            .voiceActivation = voice.mode == Config::VoiceSettings::kVoiceActivation,
            .activationLevel = static_cast<float>(voice.activationLevel) / 100.0F };
        if (!s_configured || settings != s_settings)
        {
            if (settings.active != s_settings.active || !s_configured)
            {
                logs::info("Voice {}: mode={} route={}", settings.active ? "is on" : "is off",
                    settings.voiceActivation ? "voice activation" : "push to talk",
                    route == Route::kDatagrams ? "datagrams" : route == Route::kWebSocket ? "WebSocket" : "none");
            }
            s_configured = true;
            s_settings = settings;
            Audio::Configure(settings);
        }

        const auto key = config->controls.microphoneKey;
        const auto presses = KeyState::GetPresses(key);
        if (key != s_microphoneKey)
        {
            s_microphoneKey = key;
            s_presses = presses;
        }
        if (presses != s_presses)
        {
            if (settings.voiceActivation && settings.active && (presses - s_presses) % 2 == 1)
            {
                s_microphoneOff = !s_microphoneOff;
                logs::debug("Voice microphone turned {} by its key", s_microphoneOff ? "off" : "on");
            }
            s_presses = presses;
        }
        const bool allowed = settings.active && s_inWorld && !(settings.voiceActivation && s_microphoneOff);
        Audio::SetTransmission(allowed, !settings.voiceActivation && KeyState::IsHeld(key));
    }

    void UpdateLocal(bool a_inWorld)
    {
        s_inWorld = a_inWorld;
    }

    void UpdatePeers(std::unordered_map<Multiplayer::PeerSlot, PeerState>& a_peers, RE::PlayerCharacter& a_player,
        RE::TESObjectCELL& a_cell, float a_delta)
    {
        PROFILE_FUNCTION();
        s_speakers.clear();
        if (!GameThread::Require("VoiceSync::UpdatePeers"))
        {
            return;
        }
        const auto radius = static_cast<float>(Network::VoiceTransport::GetRadius());
        const bool active = s_settings.active && radius > 0.0F;
        if (active)
        {
            Audio::CopySpeakerLevels(s_levels);
            std::ranges::sort(s_levels, {}, &Audio::SpeakerLevel::slot);
        }
        else
        {
            s_levels.clear();
        }

        const auto delta = FrameTime::Clamp(a_delta);
        const bool lipSync = Config::Get()->voice.lipSync;
        const auto playerPosition = a_player.GetPosition();
        auto listener = playerPosition;
        // Gamebryo's NiCamera looks along its local X axis with Z to its right
        // (NiCamera::GetWorldRightVector), so the rotation's third column points right on screen.
        RE::NiPoint3 right{ 0.0F, 0.0F, 0.0F };
        if (const auto* camera = RE::Main::WorldRootCamera())
        {
            listener = camera->world.translate;
            const auto& rotate = camera->world.rotate;
            right = RE::NiPoint3{ rotate.entry[0][2], rotate.entry[1][2], rotate.entry[2][2] };
        }
        const RE::NiPoint3 listenerHead{ playerPosition.x, playerPosition.y, playerPosition.z + kListenerHeadHeight };
        const FilterRevision revision{ .roster = ChatRoster::GetRevision(), .filters = PlayerFilters::GetRevision() };
        std::uint32_t rays = 0;

        for (auto& [playerID, peer] : a_peers)
        {
            auto& state = peer.voice;
            const auto& remote = peer.lifecycle.actor;
            const auto actor = remote && remote->ready ? remote->handle.get() : nullptr;
            if (!actor)
            {
                state.mouthOpen = false;
                continue;
            }
            if (!active)
            {
                CloseMouth(state, actor.get());
                continue;
            }
            RefreshFilter(state, playerID, revision);
            const auto position = actor->GetPosition();
            const auto gain = state.muted ? 0.0F : Attenuate(playerPosition.GetDistance(position), radius) * state.volume;
            const auto level = gain > 0.0F ? FindLevel(playerID) : 0.0F;
            if (gain > 0.0F)
            {
                state.occlusionCheckRemaining -= delta;
                if (state.occlusionCheckRemaining <= 0.0F && rays < kMaximumRaysPerPass)
                {
                    ++rays;
                    state.occlusionCheckRemaining = level > 0.0F ? kSpeakingOcclusionSeconds : kSilentOcclusionSeconds;
                    const RE::NiPoint3 speakerHead{ position.x, position.y, position.z + kSpeakerHeadHeight };
                    state.occluded = VoiceOcclusion::IsBlocked(a_cell, listenerHead, speakerHead);
                }
                auto direction = position - listener;
                const auto length = direction.Length();
                const auto pan = length > 1.0F ? std::clamp(direction.Dot(right) / length, -1.0F, 1.0F) : 0.0F;
                s_speakers.push_back(Audio::Speaker{
                    .slot = playerID, .gain = gain, .pan = pan, .occlusion = state.occluded ? 1.0F : 0.0F });
            }

            if (lipSync && level > kSpeakingLevel)
            {
                ActorLipSync::Apply(*actor, level);
                state.mouthOpen = true;
            }
            else
            {
                CloseMouth(state, actor.get());
            }
        }
        Audio::SetSpeakers(s_speakers);
    }

    float GetSpeakingLevel(Multiplayer::PeerSlot a_playerID)
    {
        if (!GameThread::IsAuthoritative())
        {
            return 0.0F;
        }
        const auto level = FindLevel(a_playerID);
        return level > kSpeakingLevel ? level : 0.0F;
    }

    LocalStatus GetLocalStatus()
    {
        return LocalStatus{ .active = s_settings.active,
            .microphoneOpen = Audio::IsInputOpen(),
            .transmitting = Audio::IsTransmitting(),
            .voiceActivation = s_settings.voiceActivation,
            .microphoneOff = s_settings.voiceActivation && s_microphoneOff,
            .inputLevel = Audio::GetInputLevel(),
            .route = Network::VoiceTransport::GetRoute() };
    }

    void CopyDeviceNames(bool a_capture, std::vector<std::string>& a_names)
    {
        Audio::CopyDeviceNames(a_capture, a_names);
    }

    void RefreshDevices()
    {
        Audio::RefreshDevices();
    }
}
