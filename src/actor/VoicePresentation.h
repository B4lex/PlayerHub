#pragma once

#include <RE/Skyrim.h>

#include <cstdint>

struct RemoteActor;

namespace VoicePresentation
{
    // Follows the actor's voice state, not its graph, so a graph reset leaves it.
    struct State
    {
        // From a started shout until the actor's voice state is idle again.
        bool held{ false };
        // Mirrors the registry's voice ownership.
        bool owned{ false };
        // Mirrors the registry's voice fire suppression.
        bool fireSuppressed{ false };
        // The source shout the actor holds or last held, which it does not start again.
        std::uint32_t shoutID{ 0 };
        RE::TESShout* shout{ nullptr };
        float heldElapsed{ 0.0F };
        // How long a released shout has outlasted the source's.
        float outlastedElapsed{ 0.0F };
        float startCooldown{ 0.0F };
    };

    struct ShoutRequest
    {
        // The source's shout or its stand-in; null while unresolved.
        RE::TESShout* shout{ nullptr };
        // VoiceSync's count of the source's shouts.
        std::uint32_t shoutID{ 0 };
        bool shouting{ false };
        bool released{ false };
        bool failed{ false };
        std::uint8_t words{ 0 };
    };

    void ApplyShout(RemoteActor& a_remote, const ShoutRequest& a_request, bool a_mayStart, float a_deltaTime);
    // Ends a charging shout and the voice ownership, for an actor that leaves the stage.
    void Release(RemoteActor& a_remote);
    [[nodiscard]] bool CastPower(RemoteActor& a_remote, RE::SpellItem& a_power);
    [[nodiscard]] bool IsShouting(const RemoteActor& a_remote);
}
