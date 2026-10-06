#pragma once

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Multiplayer
{
    struct FormReference
    {
        static constexpr std::size_t kMaximumPluginNameLength = 260;
        static constexpr std::uint32_t kMaximumLocalFormID = 0x00FFFFFF;

        std::string plugin;
        std::uint32_t localFormID{ 0 };

        [[nodiscard]] bool IsEmpty() const noexcept { return plugin.empty(); }

        bool operator==(const FormReference&) const = default;
        auto operator<=>(const FormReference&) const = default;
    };

    [[nodiscard]] inline bool IsValidFormReference(const FormReference& a_reference, bool a_allowEmpty)
    {
        if (a_reference.IsEmpty())
        {
            return a_allowEmpty && a_reference.localFormID == 0;
        }
        return a_reference.plugin.size() <= FormReference::kMaximumPluginNameLength
            && a_reference.localFormID <= FormReference::kMaximumLocalFormID;
    }

    namespace Detail
    {
        [[nodiscard]] constexpr char ToLowerAscii(char a_character) noexcept
        {
            return a_character >= 'A' && a_character <= 'Z' ? static_cast<char>(a_character - 'A' + 'a') : a_character;
        }

        // Plugin names compare without case, as the game's lookup does.
        [[nodiscard]] constexpr bool PluginNameLess(std::string_view a_left, std::string_view a_right) noexcept
        {
            return std::ranges::lexicographical_compare(a_left, a_right,
                [](char a_first, char a_second) { return ToLowerAscii(a_first) < ToLowerAscii(a_second); });
        }

        // The Skyrim Special Edition masters and the Creation Club plugins
        // Anniversary Edition bundles, as its Skyrim.ccc lists them.
        inline constexpr std::array<std::string_view, 80> kBaseGamePlugins{
            "_ResourcePack.esl",
            "ccAFDSSE001-DweSanctuary.esm",
            "ccASVSSE001-ALMSIVI.esm",
            "ccBGSSSE001-Fish.esm",
            "ccBGSSSE002-ExoticArrows.esl",
            "ccBGSSSE003-Zombies.esl",
            "ccBGSSSE004-RuinsEdge.esl",
            "ccBGSSSE005-Goldbrand.esl",
            "ccBGSSSE006-StendarsHammer.esl",
            "ccBGSSSE007-Chrysamere.esl",
            "ccBGSSSE008-Wraithguard.esl",
            "ccBGSSSE010-PetDwarvenArmoredMudcrab.esl",
            "ccBGSSSE011-HrsArmrElvn.esl",
            "ccBGSSSE012-HrsArmrStl.esl",
            "ccBGSSSE013-Dawnfang.esl",
            "ccBGSSSE014-SpellPack01.esl",
            "ccBGSSSE016-Umbra.esm",
            "ccBGSSSE018-Shadowrend.esl",
            "ccBGSSSE019-StaffofSheogorath.esl",
            "ccBGSSSE020-GrayCowl.esl",
            "ccBGSSSE021-LordsMail.esl",
            "ccBGSSSE025-AdvDSGS.esm",
            "ccBGSSSE031-AdvCyrus.esm",
            "ccBGSSSE034-MntUni.esl",
            "ccBGSSSE035-PetNHound.esl",
            "ccBGSSSE036-PetBWolf.esl",
            "ccBGSSSE037-Curios.esl",
            "ccBGSSSE038-BowofShadows.esl",
            "ccBGSSSE040-AdvObGobs.esl",
            "ccBGSSSE041-NetchLeather.esl",
            "ccBGSSSE043-CrossElv.esl",
            "ccBGSSSE045-Hasedoki.esl",
            "ccBGSSSE050-BA_Daedric.esl",
            "ccBGSSSE051-BA_DaedricMail.esl",
            "ccBGSSSE052-BA_Iron.esl",
            "ccBGSSSE053-BA_Leather.esl",
            "ccBGSSSE054-BA_Orcish.esl",
            "ccBGSSSE055-BA_OrcishScaled.esl",
            "ccBGSSSE056-BA_Silver.esl",
            "ccBGSSSE057-BA_Stalhrim.esl",
            "ccBGSSSE058-BA_Steel.esl",
            "ccBGSSSE059-BA_Dragonplate.esl",
            "ccBGSSSE060-BA_Dragonscale.esl",
            "ccBGSSSE061-BA_Dwarven.esl",
            "ccBGSSSE062-BA_DwarvenMail.esl",
            "ccBGSSSE063-BA_Ebony.esl",
            "ccBGSSSE064-BA_Elven.esl",
            "ccBGSSSE066-Staves.esl",
            "ccBGSSSE067-DaedInv.esm",
            "ccBGSSSE068-Bloodfall.esl",
            "ccBGSSSE069-Contest.esl",
            "ccCBHSSE001-Gaunt.esl",
            "ccEDHSSE001-NorJewel.esl",
            "ccEDHSSE002-SplKntSet.esl",
            "ccEDHSSE003-Redguard.esl",
            "ccEEJSSE001-Hstead.esm",
            "ccEEJSSE002-Tower.esl",
            "ccEEJSSE003-Hollow.esl",
            "ccEEJSSE004-Hall.esl",
            "ccEEJSSE005-Cave.esm",
            "ccFFBSSE001-ImperialDragon.esl",
            "ccFFBSSE002-CrossbowPack.esl",
            "ccFSVSSE001-Backpacks.esl",
            "ccKRTSSE001_Altar.esl",
            "ccMTYSSE001-KnightsoftheNine.esl",
            "ccMTYSSE002-VE.esl",
            "ccPEWSSE002-ArmsOfChaos.esl",
            "ccQDRSSE001-SurvivalMode.esl",
            "ccQDRSSE002-Firewood.esl",
            "ccRMSSSE001-NecroHouse.esl",
            "ccTWBSSE001-PuzzleDungeon.esm",
            "ccVSVSSE001-Winter.esl",
            "ccVSVSSE002-Pets.esl",
            "ccVSVSSE003-NecroArts.esl",
            "ccVSVSSE004-BeAFarmer.esl",
            "Dawnguard.esm",
            "Dragonborn.esm",
            "HearthFires.esm",
            "Skyrim.esm",
            "Update.esm",
        };
        static_assert(std::ranges::is_sorted(kBaseGamePlugins, PluginNameLess));
    }

    // Plugins every Anniversary Edition install ships with. A Special Edition
    // install without the Anniversary upgrade has only the masters.
    [[nodiscard]] constexpr bool IsBaseGamePlugin(std::string_view a_plugin) noexcept
    {
        return std::ranges::binary_search(Detail::kBaseGamePlugins, a_plugin, Detail::PluginNameLess);
    }
}
