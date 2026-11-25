#include "sync/voicechat/VoiceChatSync.h"
#include "actor/ActorLipSync.h"
#include "actor/VoiceOcclusion.h"
#include "audio/AudioEngine.h"
#include "config/PluginConfig.h"
#include "network/VoiceTransport.h"
#include "runtime/FrameTime.h"
#include "runtime/GameSession.h"
#include "runtime/GameThread.h"
#include "runtime/KeyState.h"
#include "runtime/PluginTime.h"
#include "runtime/Profiling.h"
#include "sync/PeerState.h"
#include "sync/chat/ChatRoster.h"
#include "sync/filters/PlayerFilters.h"
#include "sync/movement/MovementSync.h"

#include <algorithm>
#include <cmath>

namespace VoiceChatSync
{
    namespace
    {
        using Network::VoiceTransport::State;

        // Full volume within about four metres, then an inverse-distance fall that is gentler than
        // the physical one, so a voice carries across a camp; the last fifth of the radius fades to
        // nothing, where the relay stops sending.
        constexpr float kFullVolumeDistance = 300.0F;
        constexpr float kRolloffExponent = 0.7F;
        constexpr float kFadeFraction = 0.2F;
        // Vanilla's dialogue output models send 20 to 30% to the game's reverb. The send falls with
        // the square root of the direct rolloff, so a farther voice is heard more through the room.
        constexpr float kReverbSend = 0.2F;
        constexpr float kReverbRolloffExponent = 0.5F;
        // Heights of the ray between the listener's and the speaker's heads above their origins.
        constexpr float kListenerHeadHeight = 120.0F;
        constexpr float kSpeakerHeadHeight = 110.0F;
        constexpr float kSpeakingOcclusionSeconds = 0.25F;
        constexpr float kSilentOcclusionSeconds = 1.0F;
        // A cost cap per main-loop pass, not a duration.
        constexpr std::uint32_t kMaximumRaysPerPass = 4;
        constexpr float kSpeakingLevel = 0.05F;
        constexpr float kBaseInputGain = 1.4F;

        Audio::Settings s_settings;
        bool s_configured = false;
        bool s_inWorld = false;
        bool s_localMouthOpen = false;

        std::vector<Audio::Speaker> s_speakers;
        std::vector<Audio::SpeakerLevel> s_levels;

        struct Loudness
        {
            float direct{ 0.0F };
            float reverbSend{ 0.0F };
        };

        Loudness Attenuate(float a_distance, float a_radius)
        {
            if (a_distance >= a_radius)
            {
                return {};
            }
            const auto rolloff = a_distance <= kFullVolumeDistance ? 1.0F : kFullVolumeDistance / a_distance;
            const auto fade = std::clamp((a_radius - a_distance) / (a_radius * kFadeFraction), 0.0F, 1.0F);
            return Loudness{ .direct = std::pow(rolloff, kRolloffExponent) * fade,
                .reverbSend = kReverbSend * std::pow(rolloff, kReverbRolloffExponent) * fade };
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
            a_state.silenced
                = player && (PlayerFilters::IsMuted(player->session) || PlayerFilters::IsVanished(player->session));
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
        const auto state = Network::VoiceTransport::GetState();
        // Squared, so the slider reads close to loudness as heard: twice as loud takes about 10 dB.
        const auto inputVolume = static_cast<float>(voice.inputVolume) / 100.0F;
        const bool input = voice.inputDevice != Config::VoiceSettings::kNoInputDevice;
        const Audio::Settings settings{ .active = state == State::kConnected && GameSession::IsInGame(),
            .input = input,
            .inputDevice = input ? voice.inputDevice : std::string{},
            .outputDevice = voice.outputDevice,
            .inputGain = kBaseInputGain * inputVolume * inputVolume,
            .outputGain = static_cast<float>(voice.outputVolume) / 100.0F,
            .noiseSuppression = voice.noiseSuppression,
            .voiceActivation = voice.mode == Config::VoiceSettings::kVoiceActivation,
            .activationLevel = static_cast<float>(voice.activationLevel) / 100.0F };
        if (!s_configured || settings != s_settings)
        {
            if (settings.active)
            {
                if (!s_configured || !s_settings.active)
                {
                    logs::info("Voice is on: mode={}", !settings.input         ? "no microphone"
                                                       : settings.voiceActivation ? "voice activation"
                                                                                  : "push to talk");
                }
            }
            else if (s_configured && s_settings.active)
            {
                const auto* relay = state == State::kConnected     ? "connected"
                                  : state == State::kConnecting  ? "connecting"
                                  : state == State::kUnreachable ? "unreachable"
                                                                 : "none";
                logs::info("Voice is off: relay={}", relay);
            }
            s_configured = true;
            s_settings = settings;
            Audio::Configure(settings);
        }

        Audio::SetTransmission(settings.active && s_inWorld,
            !settings.voiceActivation && KeyState::IsHeld(config->controls.microphoneKey));
    }

    void UpdateLocal(RE::PlayerCharacter* a_player, bool a_inWorld)
    {
        s_inWorld = a_inWorld;
        if (!a_player)
        {
            return;
        }
        const auto level = a_inWorld ? Audio::GetSentLevel() : 0.0F;
        if (level > kSpeakingLevel)
        {
            ActorLipSync::Apply(*a_player, level);
            s_localMouthOpen = true;
        }
        else if (s_localMouthOpen)
        {
            ActorLipSync::Apply(*a_player, 0.0F);
            s_localMouthOpen = false;
        }
    }

    void UpdatePeers(std::unordered_map<Multiplayer::PeerSlot, PeerState>& a_peers, RE::PlayerCharacter& a_player,
        RE::TESObjectCELL& a_cell, float a_delta)
    {
        PROFILE_FUNCTION();
        PLUGIN_TIME_STAGE("Voice");
        s_speakers.clear();
        if (!GameThread::Require("VoiceChatSync::UpdatePeers"))
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
        const auto playerPosition = a_player.GetPosition();
        auto listener = playerPosition;
        // Gamebryo's NiCamera looks along its local X axis with Z to its right
        // (NiCamera::GetWorldRightVector), so the rotation's first column is the view direction and
        // its third points right on screen.
        RE::NiPoint3 right{ 0.0F, 0.0F, 0.0F };
        RE::NiPoint3 forward{ 0.0F, 0.0F, 0.0F };
        if (const auto* camera = RE::Main::WorldRootCamera())
        {
            listener = camera->world.translate;
            const auto& rotate = camera->world.rotate;
            right = RE::NiPoint3{ rotate.entry[0][2], rotate.entry[1][2], rotate.entry[2][2] };
            forward = RE::NiPoint3{ rotate.entry[0][0], rotate.entry[1][0], rotate.entry[2][0] };
        }
        const RE::NiPoint3 listenerHead{ playerPosition.x, playerPosition.y, playerPosition.z + kListenerHeadHeight };
        const FilterRevision revision{ .roster = ChatRoster::GetRevision(), .filters = PlayerFilters::GetRevision() };
        std::uint32_t rays = 0;

        const auto localCellID = a_cell.GetFormID();
        const auto localWorldspaceID = MovementSync::GetWorldspaceID(a_player);
        for (auto& [playerID, peer] : a_peers)
        {
            auto& state = peer.voiceChat;
            const auto& remote = peer.lifecycle.actor;
            const auto actor = remote && remote->ready ? remote->handle.get() : nullptr;
            RE::NiPoint3 position;
            if (actor)
            {
                position = actor->GetPosition();
            }
            else
            {
                state.mouthOpen = false;
                if (!MovementSync::IsSameCoordinateSpace(
                        peer.space.cellID, peer.space.worldspaceID, localCellID, localWorldspaceID))
                {
                    continue;
                }
                position = peer.playout.GetPose().position;
            }
            if (!active)
            {
                CloseMouth(state, actor.get());
                continue;
            }
            RefreshFilter(state, playerID, revision);
            const auto distance = playerPosition.GetDistance(position);
            const auto loudness = state.silenced ? Loudness{} : Attenuate(distance, radius);
            const auto gain = loudness.direct;
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
                const auto rear = length > 1.0F ? std::clamp(-direction.Dot(forward) / length, 0.0F, 1.0F) : 0.0F;
                const auto air = radius > kFullVolumeDistance
                                   ? std::clamp((distance - kFullVolumeDistance) / (radius - kFullVolumeDistance), 0.0F, 1.0F)
                                   : 0.0F;
                s_speakers.push_back(Audio::Speaker{ .slot = playerID,
                    .gain = gain,
                    .pan = pan,
                    .occlusion = state.occluded ? 1.0F : 0.0F,
                    .reverbSend = loudness.reverbSend,
                    .air = air,
                    .rear = rear });
            }

            if (!actor)
            {
                continue;
            }
            if (level > kSpeakingLevel)
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

    bool IsLocalMouthOpen()
    {
        return s_localMouthOpen;
    }

    bool IsSpeaking(Multiplayer::PeerSlot a_playerID)
    {
        return GameThread::IsAuthoritative() && FindLevel(a_playerID) > kSpeakingLevel;
    }

    LocalStatus GetLocalStatus()
    {
        return LocalStatus{ .active = s_settings.active,
            .microphoneOpen = Audio::IsInputOpen(),
            .transmitting = Audio::IsTransmitting(),
            .voiceActivation = s_settings.voiceActivation,
            .inputLevel = Audio::GetInputLevel(),
            .relayUnreachable = Network::VoiceTransport::GetState() == State::kUnreachable };
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
