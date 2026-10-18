#include "runtime/VanillaIntro.h"
#include "runtime/FrameTime.h"
#include "runtime/GameSession.h"

#include <atomic>
#include <cstdint>
#include <string_view>

namespace VanillaIntro
{
    namespace
    {
        constexpr std::string_view kQuestPlugin = "Skyrim.esm";
        constexpr RE::FormID kQuestLocalFormID = 0x0003372B; // MQ101
        // QF_MQ101_0003372B Fragment_316 unbinds the player, enables movement, and shows the move help after
        // Alduin's knockdown (stages 140 and 150).
        constexpr std::uint16_t kPlayerControlStage = 160;
        // SKSE sends kNewGame after EnsureQuestStarted (ID 25003) returns, which may only queue the start.
        constexpr float kStartWaitSeconds = 5.0F;

        std::atomic_bool s_playing{ false };
        std::atomic_bool s_checking{ false };
        std::atomic_bool s_awaitingStart{ false };
        std::atomic_bool s_restartWait{ false };

        // Main-loop thread only. A Skyrim.esm form, which lives as long as the process.
        const RE::TESQuest* s_quest = nullptr;
        float s_awaitedSeconds = 0.0F;

        bool AwaitStart(bool a_running, float a_deltaSeconds)
        {
            if (!s_awaitingStart.load(std::memory_order_acquire))
            {
                return false;
            }
            if (s_restartWait.exchange(false, std::memory_order_acq_rel))
            {
                s_awaitedSeconds = 0.0F;
            }
            if (a_running || s_quest->IsCompleted())
            {
                s_awaitingStart.store(false, std::memory_order_release);
                return false;
            }
            if (!GameSession::IsInGame())
            {
                return true;
            }
            s_awaitedSeconds += FrameTime::Clamp(a_deltaSeconds);
            if (s_awaitedSeconds < kStartWaitSeconds)
            {
                return true;
            }
            s_awaitingStart.store(false, std::memory_order_release);
            logs::info("MQ101 did not start within {:.0f} seconds in the world after a new game", kStartWaitSeconds);
            return false;
        }
    }

    void Initialize()
    {
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        s_quest = dataHandler ? dataHandler->LookupForm<RE::TESQuest>(kQuestLocalFormID, kQuestPlugin) : nullptr;
        if (!s_quest)
        {
            logs::warn("Vanilla intro quest MQ101 was not found; the intro never holds the connection");
        }
    }

    void OnNewGame(const void* a_startQuest)
    {
        const bool vanilla = s_quest && a_startQuest == s_quest;
        s_restartWait.store(true, std::memory_order_release);
        s_awaitingStart.store(vanilla, std::memory_order_release);
        s_checking.store(true, std::memory_order_release);
        logs::info("New game: start quest {} MQ101", vanilla ? "is" : "is not");
    }

    void OnGameLoadStarted()
    {
        // The quest's state is the previous game's or partly loaded until kPostLoadGame.
        s_checking.store(false, std::memory_order_release);
        s_awaitingStart.store(false, std::memory_order_release);
    }

    void OnGameLoaded()
    {
        s_checking.store(true, std::memory_order_release);
    }

    void Update(float a_deltaSeconds)
    {
        if (!s_quest || !s_checking.load(std::memory_order_acquire))
        {
            return;
        }
        const bool running = s_quest->IsEnabled();
        const auto stage = s_quest->GetCurrentStageID();
        const bool awaiting = AwaitStart(running, a_deltaSeconds);
        const bool playing = awaiting || (running && stage < kPlayerControlStage);
        const bool changed = s_playing.exchange(playing, std::memory_order_acq_rel) != playing;
        if (!playing)
        {
            s_checking.store(false, std::memory_order_release);
        }
        if (changed || !playing)
        {
            logs::info("Vanilla intro hold {}: MQ101 running={} stage={} awaitingStart={}", playing ? "on" : "off",
                running, stage, awaiting);
        }
    }

    bool IsPlaying()
    {
        // The last game's hold stays set in the main menu until the next game checks again.
        return s_playing.load(std::memory_order_acquire) && GameSession::IsInGame();
    }
}
