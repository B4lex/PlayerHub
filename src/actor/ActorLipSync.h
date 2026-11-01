#pragma once

#include <RE/Skyrim.h>

namespace ActorLipSync
{
    // Opens the actor's mouth by a_openness, 0 to 1, through the phoneme keyframe the console's
    // `mfg phoneme` sets; 0 closes it. Game thread.
    void Apply(RE::Actor& a_actor, float a_openness);
}
