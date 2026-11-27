#include "actor/RaceMenuSliderCatalog.h"
#include "protocol/AppearanceState.h"
#include "runtime/Profiling.h"
#include "text/ASCIICase.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace RaceMenuSliderCatalog
{
    namespace
    {
        // SKEE's FaceMorphInterface::LoadMods reads <folder><plugin>\races.ini for every
        // active plugin. Each line names a race by editor ID and the slider files it
        // uses, relative to the plugin's folder, or to the folder itself after ':'.
        constexpr std::string_view kMorphFolder = "Meshes\\actors\\character\\FaceGenMorphs\\";
        constexpr std::size_t kMaximumReportedNames = 32;

        // ReadSliders accepts these categories (-1 stands for Extra) and skips a line
        // with any other.
        constexpr std::array kCategories{ -1, 4, 8, 16, 32, 64, 128, 256, 512, 1024 };

        using SexSliders = std::array<std::vector<Slider>, 2>;

        // The names a slider file defines for each sex, in file order.
        using SliderFile = std::array<std::vector<std::string>, 2>;

        struct Catalog
        {
            std::unordered_map<const RE::TESRace*, SexSliders> races;
            std::vector<Slider> all;
            bool built{ false };
        };

        Catalog s_catalog;

        std::string_view Trim(std::string_view a_text)
        {
            constexpr std::string_view kSpace = " \t\r\n";
            const auto first = a_text.find_first_not_of(kSpace);
            if (first == std::string_view::npos)
            {
                return {};
            }
            return a_text.substr(first, a_text.find_last_not_of(kSpace) - first + 1);
        }

        std::string ToLower(std::string_view a_text)
        {
            std::string result{ a_text };
            std::ranges::transform(result, result.begin(), ASCIICase::ToLower);
            return result;
        }

        std::vector<std::string_view> Split(std::string_view a_text, char a_separator)
        {
            std::vector<std::string_view> parts;
            std::size_t start = 0;
            while (true)
            {
                const auto end = a_text.find(a_separator, start);
                parts.push_back(Trim(a_text.substr(start, end - start)));
                if (end == std::string_view::npos)
                {
                    return parts;
                }
                start = end + 1;
            }
        }

        std::optional<std::vector<std::string>> ReadLines(const std::string& a_path)
        {
            RE::BSResourceNiBinaryStream stream{ a_path };
            if (!stream.good())
            {
                return std::nullopt;
            }
            std::vector<std::string> lines;
            std::string line;
            while (std::getline(stream, line, '\n'))
            {
                lines.push_back(line);
            }
            return lines;
        }

        void ReportSkippedName(std::string_view a_name, const std::string& a_path)
        {
            static std::vector<std::string> reported;
            if (reported.size() >= kMaximumReportedNames || std::ranges::find(reported, a_name) != reported.end())
            {
                return;
            }
            reported.emplace_back(a_name);
            logs::warn("RaceMenu slider catalog skipped a slider peers would reject: name=\"{}\" file={}", a_name,
                a_path);
        }

        // Follows SKEE's FaceMorphInterface::ReadSliders: lines before a [Male] or
        // [Female] section are male, and HeadPart sliders choose head parts, which
        // sync on their own.
        SliderFile ReadSliderFile(const std::string& a_path)
        {
            SliderFile result;
            const auto lines = ReadLines(a_path);
            if (!lines)
            {
                logs::debug("RaceMenu slider file not found: {}", a_path);
                return result;
            }
            std::size_t sex = 0;
            for (const auto& rawLine : *lines)
            {
                const auto line = Trim(rawLine);
                if (line.empty() || line.front() == '#')
                {
                    continue;
                }
                if (line.front() == '[')
                {
                    const auto section = line.substr(1);
                    if (ASCIICase::StartsWith(section, "Male"))
                    {
                        sex = 0;
                    }
                    else if (ASCIICase::StartsWith(section, "Female"))
                    {
                        sex = 1;
                    }
                    continue;
                }
                const auto sides = Split(line, '=');
                if (sides.size() < 2)
                {
                    continue;
                }
                const auto params = Split(sides[1], ',');
                if (params.size() < 4 || params[0].empty())
                {
                    continue;
                }
                int category = 0;
                const auto parsed = std::from_chars(params[0].data(), params[0].data() + params[0].size(), category);
                const bool valueType = ASCIICase::StartsWith(params[1], "Slider") || ASCIICase::StartsWith(params[1], "Preset");
                if (parsed.ec != std::errc{} || !std::ranges::contains(kCategories, category) || !valueType)
                {
                    continue;
                }
                const auto name = sides[0];
                if (!Multiplayer::IsValidFaceSliderName(name))
                {
                    ReportSkippedName(name, a_path);
                    continue;
                }
                result[sex].emplace_back(name);
            }
            return result;
        }

        std::unordered_map<std::string, const RE::TESRace*> MapRacesByEditorID(RE::TESDataHandler& a_dataHandler)
        {
            std::unordered_map<std::string, const RE::TESRace*> races;
            for (const auto* race : a_dataHandler.GetFormArray<RE::TESRace>())
            {
                const char* editorID = race ? race->formEditorID.c_str() : nullptr;
                if (editorID && *editorID)
                {
                    races.emplace(ToLower(editorID), race);
                }
            }
            return races;
        }

        std::vector<const RE::TESFile*> GetActivePlugins(const RE::TESDataHandler& a_dataHandler)
        {
            std::vector<const RE::TESFile*> plugins;
            const auto append = [&plugins](const RE::TESFile* const* a_files, std::size_t a_count) {
                for (std::size_t i = 0; i < a_count; ++i)
                {
                    if (a_files[i])
                    {
                        plugins.push_back(a_files[i]);
                    }
                }
            };
            append(a_dataHandler.GetLoadedMods(), a_dataHandler.GetLoadedModCount());
            append(a_dataHandler.GetLoadedLightMods(), a_dataHandler.GetLoadedLightModCount());
            return plugins;
        }

        void Build()
        {
            PROFILE_FUNCTION();
            s_catalog.built = true;
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler)
            {
                logs::warn("RaceMenu slider catalog not built: no data handler");
                return;
            }

            const auto racesByEditorID = MapRacesByEditorID(*dataHandler);
            std::map<std::string, SliderFile> files;
            // SKEE's value maps compare names without case, so the first spelling stands
            // for a race and sex.
            std::unordered_map<const RE::TESRace*, std::array<std::map<std::string, std::string>, 2>> names;
            std::size_t raceFiles = 0;
            for (const auto* plugin : GetActivePlugins(*dataHandler))
            {
                const auto pluginFolder = std::string{ plugin->GetFilename() } + "\\";
                const auto racesPath = std::string{ kMorphFolder } + pluginFolder + "races.ini";
                const auto lines = ReadLines(racesPath);
                if (!lines)
                {
                    continue;
                }
                ++raceFiles;
                for (const auto& rawLine : *lines)
                {
                    const auto line = Trim(rawLine);
                    const auto sides = Split(line, '=');
                    if (line.empty() || line.front() == '#' || sides.size() < 2)
                    {
                        continue;
                    }
                    const auto race = racesByEditorID.find(ToLower(sides[0]));
                    for (auto file : Split(sides[1], ','))
                    {
                        if (file.empty() || race == racesByEditorID.end())
                        {
                            continue;
                        }
                        const bool fromFolder = file.front() == ':';
                        auto path = std::string{ kMorphFolder } + (fromFolder ? "" : pluginFolder)
                            + std::string{ fromFolder ? file.substr(1) : file };
                        auto parsed = files.find(path);
                        if (parsed == files.end())
                        {
                            parsed = files.emplace(path, ReadSliderFile(path)).first;
                        }
                        auto& raceNames = names[race->second];
                        for (std::size_t sex = 0; sex < 2; ++sex)
                        {
                            for (const auto& name : parsed->second[sex])
                            {
                                raceNames[sex].try_emplace(ToLower(name), name);
                            }
                        }
                    }
                }
            }

            std::size_t sliderCount = 0;
            std::map<std::string, std::string> allNames;
            for (auto& [race, sexes] : names)
            {
                auto& entry = s_catalog.races[race];
                for (std::size_t sex = 0; sex < 2; ++sex)
                {
                    std::set<std::string> sorted;
                    for (auto& [key, name] : sexes[sex])
                    {
                        allNames.try_emplace(key, name);
                        sorted.insert(std::move(name));
                    }
                    for (const auto& name : sorted)
                    {
                        entry[sex].push_back(Slider{ .name = name, .key = SKEEInterfaces::SKEEString{ name } });
                    }
                    sliderCount += entry[sex].size();
                }
            }
            for (const auto& [key, name] : allNames)
            {
                s_catalog.all.push_back(Slider{ .name = name, .key = SKEEInterfaces::SKEEString{ name } });
            }
            logs::info("RaceMenu slider catalog built: raceFiles={} sliderFiles={} races={} sliders={} names={}",
                raceFiles, files.size(), s_catalog.races.size(), sliderCount, s_catalog.all.size());
        }
    }

    const std::vector<Slider>& Get(const RE::TESRace& a_race, bool a_female)
    {
        static const std::vector<Slider> none;
        if (!s_catalog.built)
        {
            Build();
        }
        const auto found = s_catalog.races.find(&a_race);
        return found == s_catalog.races.end() ? none : found->second[a_female ? 1 : 0];
    }

    const std::vector<Slider>& GetAll()
    {
        if (!s_catalog.built)
        {
            Build();
        }
        return s_catalog.all;
    }
}
