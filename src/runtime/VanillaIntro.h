#pragma once

namespace VanillaIntro
{
    void Initialize();

    void OnNewGame(const void* a_startQuest);

    void OnGameLoadStarted();

    void OnGameLoaded();

    void Update(float a_deltaSeconds);

    [[nodiscard]] bool IsPlaying();
}
