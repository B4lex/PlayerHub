#pragma once

// Whether a game is loaded: from the first time the player is placed in the
// world until the main menu opens. Loading screens in between, including
// loading another save from in game, keep the game.
namespace GameSession
{
    // Safe to call from any thread.
    void SetInGame(bool a_inGame);
    [[nodiscard]] bool IsInGame();
}
