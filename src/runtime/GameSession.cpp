#include "runtime/GameSession.h"

#include <atomic>

namespace GameSession
{
    namespace
    {
        std::atomic_bool inGame{ false };
    }

    void SetInGame(bool a_inGame)
    {
        // Set on every main-loop pass in the world, so the common case only loads.
        if (inGame.load(std::memory_order_relaxed) == a_inGame
            || inGame.exchange(a_inGame, std::memory_order_acq_rel) == a_inGame)
        {
            return;
        }
        logs::info("Game {}", a_inGame ? "started" : "ended");
    }

    bool IsInGame()
    {
        return inGame.load(std::memory_order_acquire);
    }
}
