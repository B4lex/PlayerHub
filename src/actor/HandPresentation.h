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
        std::array<bool, 2> releaseRequested{};
        std::array<bool, 2> released{};
        std::array<bool, 2> awaitingNewCast{};
        std::array<bool, 2> releasable{};
        std::array<float, 2> releaseElapsed{};
        std::array<float, 2> stopRequestElapsed{};
        std::array<float, 2> eventCooldown{};
    };

    struct CastRequest
    {
        bool leftHand{ false };
        bool casting{ false };
        bool released{ false };
        bool dual{ false };
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
    void ApplySpellCast(RemoteActor& a_remote, const CastRequest& a_request, bool a_mayStart, float a_deltaTime);
    void ApplyBlock(RemoteActor& a_remote, bool a_blocking, bool a_mayStart, float a_deltaTime);

    [[nodiscard]] bool IsBowOrCastPlaying(const State& a_state);
    [[nodiscard]] bool IsBlockPlaying(const State& a_state);
    [[nodiscard]] bool IsPlaying(const RemoteActor& a_remote);
}
