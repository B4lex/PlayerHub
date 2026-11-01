#include "actor/ActorLipSync.h"
#include "runtime/GameThread.h"

#include <algorithm>

namespace ActorLipSync
{
    namespace
    {
        using Phoneme = RE::BSFaceGenKeyframeMultiple::Phoneme;

        // Aah opens the jaw and BigAah the lips around it; together they read as speech at a distance
        // without a phoneme analysis of the voice.
        constexpr float kAahWeight = 0.75F;
        constexpr float kBigAahWeight = 0.45F;
    }

    void Apply(RE::Actor& a_actor, float a_openness)
    {
        if (!GameThread::Require("ActorLipSync::Apply"))
        {
            return;
        }
        auto* face = a_actor.GetFaceGenAnimationData();
        if (!face)
        {
            return;
        }
        const auto openness = std::clamp(a_openness, 0.0F, 1.0F);
        // The face update reads the keyframes under this lock on its own thread.
        RE::BSSpinLockGuard guard{ face->lock };
        auto& phonemes = face->phenomeKeyFrame;
        if (!phonemes.values || phonemes.count <= Phoneme::BigAah)
        {
            return;
        }
        phonemes.SetValue(Phoneme::Aah, openness * kAahWeight);
        phonemes.SetValue(Phoneme::BigAah, openness * openness * kBigAahWeight);
    }
}
