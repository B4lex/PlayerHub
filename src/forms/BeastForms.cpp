#include "forms/BeastForms.h"

#include <cstddef>
#include <string_view>

namespace Forms
{
    namespace
    {
        constexpr std::string_view kDawnguard = "Dawnguard.esm";

        struct BeastRace
        {
            Multiplayer::Shapeshift form;
            std::string_view plugin;
            RE::FormID localFormID;
        };

        constexpr std::array kBeastRaces{
            BeastRace{ Multiplayer::Shapeshift::kWerewolf, "Skyrim.esm", 0x0CDD84 }, // WerewolfBeastRace
            BeastRace{ Multiplayer::Shapeshift::kVampireLord, kDawnguard, 0x00283A }, // DLC1VampireBeastRace
            BeastRace{ Multiplayer::Shapeshift::kWerebear, "Dragonborn.esm", 0x01E17B }, // DLC2WerebearBeastRace
        };

        struct VampireLordPowerIDs
        {
            RE::FormID effect;
            std::array<RE::FormID, 2> shaders;
            RE::FormID art;
            float alpha;
        };

        // Dawnguard.esm. DLC1MistformEffect shows DLC1VampireMistform01FXS and
        // DLC1VampMistFormFX as its hit shader and art, and its scripts add
        // DLC1VampireMistform02FXS and an actor alpha of 0.25. DLC1BatsEffect shows
        // DLC1VampireBatsFXS and DLC1VampLordBatsFX.
        constexpr std::array kVampireLordPowers{
            VampireLordPowerIDs{ 0x01A321, { 0x01AA2E, 0x01AA30 }, 0x01AA34, 0.25F },
            VampireLordPowerIDs{ 0x01571A, { 0x018EF0, 0 }, 0x018EEF, 1.0F },
        };

        // Written once at kDataLoaded, before the main loop reads them.
        std::array<RE::TESRace*, kBeastRaces.size()> s_races{};
        std::array<VampireLordPowerForms, kVampireLordPowers.size()> s_vampireLordPowers{};

        template <class T>
        T* Lookup(RE::TESDataHandler& a_dataHandler, RE::FormID a_localFormID)
        {
            return a_localFormID != 0 ? a_dataHandler.LookupForm<T>(a_localFormID, kDawnguard) : nullptr;
        }
    }

    void ResolveBeastForms()
    {
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler)
        {
            return;
        }
        for (std::size_t i = 0; i < kBeastRaces.size(); ++i)
        {
            const auto& entry = kBeastRaces[i];
            s_races[i] = dataHandler->LookupForm<RE::TESRace>(entry.localFormID, entry.plugin);
            logs::info("Beast race {}: {}|{:06X} {}", Multiplayer::GetShapeshiftName(entry.form), entry.plugin,
                entry.localFormID, s_races[i] ? "resolved" : "missing");
        }
        for (std::size_t i = 0; i < kVampireLordPowers.size(); ++i)
        {
            const auto& ids = kVampireLordPowers[i];
            auto& forms = s_vampireLordPowers[i];
            forms.effect = Lookup<RE::EffectSetting>(*dataHandler, ids.effect);
            for (std::size_t shader = 0; shader < ids.shaders.size(); ++shader)
            {
                forms.shaders[shader] = Lookup<RE::TESEffectShader>(*dataHandler, ids.shaders[shader]);
            }
            forms.art = Lookup<RE::BGSArtObject>(*dataHandler, ids.art);
            forms.alpha = ids.alpha;
            logs::info("Vampire lord power {}: effect={:08X} shaders={:08X},{:08X} art={:08X}", i,
                forms.effect ? forms.effect->GetFormID() : 0, forms.shaders[0] ? forms.shaders[0]->GetFormID() : 0,
                forms.shaders[1] ? forms.shaders[1]->GetFormID() : 0, forms.art ? forms.art->GetFormID() : 0);
        }
    }

    RE::TESRace* GetBeastRace(Multiplayer::Shapeshift a_form)
    {
        for (std::size_t i = 0; i < kBeastRaces.size(); ++i)
        {
            if (kBeastRaces[i].form == a_form)
            {
                return s_races[i];
            }
        }
        return nullptr;
    }

    Multiplayer::Shapeshift GetShapeshift(const RE::TESRace* a_race)
    {
        for (std::size_t i = 0; i < kBeastRaces.size(); ++i)
        {
            if (a_race && s_races[i] == a_race)
            {
                return kBeastRaces[i].form;
            }
        }
        return Multiplayer::Shapeshift::kNone;
    }

    const VampireLordPowerForms& GetVampireLordPower(VampireLordPower a_power)
    {
        return s_vampireLordPowers[static_cast<std::size_t>(a_power)];
    }
}
