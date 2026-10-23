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
        constexpr RE::FormID kCuffsLocalFormID = 0x0010E039;
        // QF_MQ101_0003372B Fragment_316 unbinds the player, enables movement, and shows the move help after
        // Alduin's knockdown (stages 140 and 150).
        constexpr std::uint16_t kPlayerControlStage = 160;
        constexpr float kStartWaitSeconds = 5.0F;

        std::atomic_bool s_playing{ false };
        std::atomic_bool s_checking{ false };
        std::atomic_bool s_awaitingStart{ false };
        std::atomic_bool s_restart{ false };

        // Main-loop thread only. Skyrim.esm forms, which live as long as the process.
        const RE::TESQuest* s_quest = nullptr;
        RE::FormID s_cuffsFormID = 0;
        bool s_prisoner = false;
        bool s_raceMenuSeen = false;
        float s_awaitedSeconds = 0.0F;

        bool WearsCuffs()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            return s_cuffsFormID != 0 && player && player->GetWornArmor(s_cuffsFormID, true);
        }

        bool IsRaceMenuOpen()
        {
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->IsMenuOpen(RE::RaceSexMenu::MENU_NAME);
        }

        bool AwaitStart(bool a_decided, float a_deltaSeconds)
        {
            if (!s_awaitingStart.load(std::memory_order_acquire))
            {
                return false;
            }
            if (a_decided || s_quest->IsCompleted())
            {
                s_awaitingStart.store(false, std::memory_order_release);
                return false;
            }
            if (!GameSession::IsInGame())
            {
                return true;
            }
            if (IsRaceMenuOpen())
            {
                s_raceMenuSeen = true;
                return true;
            }
            if (s_raceMenuSeen)
            {
                s_awaitingStart.store(false, std::memory_order_release);
                return false;
            }
            s_awaitedSeconds += FrameTime::Clamp(a_deltaSeconds);
            if (s_awaitedSeconds < kStartWaitSeconds)
            {
                return true;
            }
            s_awaitingStart.store(false, std::memory_order_release);
            return false;
        }
    }

    void Initialize()
    {
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        s_quest = dataHandler ? dataHandler->LookupForm<RE::TESQuest>(kQuestLocalFormID, kQuestPlugin) : nullptr;
        const auto* cuffs
            = dataHandler ? dataHandler->LookupForm<RE::TESObjectARMO>(kCuffsLocalFormID, kQuestPlugin) : nullptr;
        s_cuffsFormID = cuffs ? cuffs->GetFormID() : 0;
        if (!s_quest || !cuffs)
        {
            s_quest = nullptr;
            logs::warn("Vanilla intro quest MQ101 or the prisoner cuffs were not found");
        }
    }

    void OnNewGame(const void* a_startQuest)
    {
        const bool vanilla = s_quest && a_startQuest == s_quest;
        s_restart.store(true, std::memory_order_release);
        s_awaitingStart.store(vanilla, std::memory_order_release);
        s_checking.store(true, std::memory_order_release);
    }

    void OnGameLoadStarted()
    {
        // The quest's state is the previous game's or partly loaded until kPostLoadGame.
        s_checking.store(false, std::memory_order_release);
        s_awaitingStart.store(false, std::memory_order_release);
    }

    void OnGameLoaded()
    {
        s_restart.store(true, std::memory_order_release);
        s_checking.store(true, std::memory_order_release);
    }

    void Update(float a_deltaSeconds)
    {
        if (!s_quest || !s_checking.load(std::memory_order_acquire))
        {
            return;
        }
        if (s_restart.exchange(false, std::memory_order_acq_rel))
        {
            s_prisoner = false;
            s_raceMenuSeen = false;
            s_awaitedSeconds = 0.0F;
        }
        const bool running = s_quest->IsEnabled();
        const auto stage = s_quest->GetCurrentStageID();
        const bool intro = running && stage < kPlayerControlStage;
        if (intro && !s_prisoner)
        {
            s_prisoner = WearsCuffs();
        }
        const bool awaiting = AwaitStart(s_prisoner || (running && !intro), a_deltaSeconds);
        const bool playing = awaiting || (intro && s_prisoner);
        const bool changed = s_playing.exchange(playing, std::memory_order_acq_rel) != playing;
        if (!playing)
        {
            s_checking.store(false, std::memory_order_release);
        }
        if (changed || !playing)
        {
            logs::debug("Vanilla intro hold {}: MQ101 running={} stage={} prisoner={} awaitingStart={}",
                playing ? "on" : "off", running, stage, s_prisoner, awaiting);
        }
    }

    bool IsPlaying()
    {
        // The last game's hold stays set in the main menu until the next game checks again.
        return s_playing.load(std::memory_order_acquire) && GameSession::IsInGame();
    }
}
