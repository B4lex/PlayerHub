#pragma once

#include "protocol/PlayerState.h"

#include <RE/Skyrim.h>

#include <cstdint>

struct RemoteActor;

namespace AttackDispatch
{
    struct State
    {
        bool pending{ false };
        bool observed{ false };
        float elapsed{ 0.0F };
        // A remote actor's attack state does not report bashes (observed in game).
        bool bash{ false };
        Network::AttackAction action{ Network::AttackAction::kNormal };
        bool power{ false };
        bool chainDispatched{ false };
        // Only reported in the attack logs.
        RE::ATTACK_STATE_ENUM loggedState{ RE::ATTACK_STATE_ENUM::kNone };
        std::uint32_t chainCount{ 0 };
        std::uint32_t chainRejections{ 0 };
    };

    [[nodiscard]] bool CanStart(const RemoteActor& a_remote, const Network::Attack& a_attack);
    [[nodiscard]] bool CanChain(const RemoteActor& a_remote, const Network::Attack& a_attack);
    [[nodiscard]] bool CanPowerUp(const RemoteActor& a_remote, const Network::Attack& a_attack);
    [[nodiscard]] bool Dispatch(RemoteActor& a_remote, const Network::Attack& a_attack);
    // An attack event sent by another module, such as a beast attack.
    void Own(RemoteActor& a_remote, const RE::Actor& a_actor);
    [[nodiscard]] bool IsPlaying(const RemoteActor& a_remote);
    void Advance(RE::Actor& a_actor, RemoteActor& a_remote, float a_deltaTime);
}
