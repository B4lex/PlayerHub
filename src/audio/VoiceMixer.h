#pragma once

#include "protocol/PeerSlot.h"
#include "protocol/VoiceFrame.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace Audio
{
    // Where and how loud a speaker is heard, from the game thread.
    struct Speaker
    {
        Multiplayer::PeerSlot slot{ 0 };
        // Linear, distance and the player's volume for the speaker together; 0 drops its frames.
        float gain{ 0.0F };
        // -1 is fully left, 1 fully right.
        float pan{ 0.0F };
        // 0 is in the open, 1 behind a wall.
        float occlusion{ 0.0F };
    };

    struct SpeakerLevel
    {
        Multiplayer::PeerSlot slot{ 0 };
        // 0 to 1, the envelope of what is played.
        float level{ 0.0F };
    };

    // Plays the voices of the speakers the game thread names: one jitter buffer and Opus decoder
    // per speaker, mixed to stereo. Push runs on the network threads, Render on the playback device's.
    class VoiceMixer
    {
    public:
        VoiceMixer();
        ~VoiceMixer();
        VoiceMixer(const VoiceMixer&) = delete;
        VoiceMixer& operator=(const VoiceMixer&) = delete;

        void Push(const Multiplayer::VoiceFrame& a_frame);

        // Speakers not named lose their buffers.
        void SetSpeakers(std::span<const Speaker> a_speakers);

        void Render(float* a_stereo, std::size_t a_frames, float a_masterGain);

        void CopyLevels(std::vector<SpeakerLevel>& a_levels);

        void Clear();

    private:
        struct Channel;

        Channel* FindChannelLocked(Multiplayer::PeerSlot a_slot);
        const Speaker* FindSpeakerLocked(Multiplayer::PeerSlot a_slot) const;

        std::mutex lock;
        std::vector<Speaker> speakers;
        std::vector<std::unique_ptr<Channel>> channels;
        std::vector<float> mix;
    };
}
