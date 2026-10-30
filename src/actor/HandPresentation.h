#pragma once

#include <array>

struct RemoteActor;

namespace HandPresentation
{
    struct Bow
    {
        bool drawHeld{ false };
        bool releasePending{ false };
        float phaseElapsed{ 0.0F };
        float releaseRequestElapsed{ 0.0F };
        float eventCooldown{ 0.0F };
    };

    // Indexed by hand: 0 right, 1 left.
    struct Cast
    {
        std::array<bool, 2> held{};
        std::array<bool, 2> stopping{};
        std::array<float, 2> stopRequestElapsed{};
        std::array<float, 2> eventCooldown{};
    };

    struct Block
    {
        bool held{ false };
        bool stopping{ false };
        bool stopAccepted{ false };
        float stopElapsed{ 0.0F };
        float eventCooldown{ 0.0F };
    };

    struct State
    {
        Bow bow;
        Cast cast;
        Block block;
    };

    void ApplyBowDraw(RemoteActor& a_remote, bool a_drawn, bool a_mayStart, float a_deltaTime);
    void ApplySpellCast(RemoteActor& a_remote, bool a_leftHand, bool a_casting, bool a_mayStart, float a_deltaTime);
    void ApplyBlock(RemoteActor& a_remote, bool a_blocking, bool a_mayStart, float a_deltaTime);

    [[nodiscard]] bool IsBowOrCastPlaying(const State& a_state);
    [[nodiscard]] bool IsBlockPlaying(const State& a_state);
    [[nodiscard]] bool IsPlaying(const RemoteActor& a_remote);
}
