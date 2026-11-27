#include "sync/look/LookPreset.h"
#include "config/PluginConfig.h"
#include "config/PluginFiles.h"
#include "text/ASCIICase.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <format>
#include <limits>
#include <system_error>

namespace LookPreset
{
    namespace
    {
        // SKEE's PresetInterface keeps its presets here, relative to the game folder.
        constexpr std::string_view kDirectory = "Data/SKSE/Plugins/CharGen/Presets";
        constexpr std::string_view kExtension = ".jslot";
        constexpr std::size_t kMaximumFileSize = 4 * 1024 * 1024;
        // SKEE writes MACRO_SWAP32('SKSE') and rejects other signatures on load.
        constexpr std::uint64_t kSignature = 0x45534B53;
        // SKEE stores an unset face part as a byte.
        constexpr std::int64_t kUnsetFacePartByte = 255;
        constexpr std::int64_t kUnsetFacePartWord = 0xFFFFFFFF;

        using JSON = nlohmann::json;

        std::filesystem::path ToPath(std::string_view a_utf8)
        {
            return std::filesystem::path{ std::u8string{ a_utf8.begin(), a_utf8.end() } };
        }

        const JSON* Find(const JSON& a_object, std::string_view a_key)
        {
            if (!a_object.is_object())
            {
                return nullptr;
            }
            const auto found = a_object.find(a_key);
            return found == a_object.end() ? nullptr : &*found;
        }

        std::optional<std::uint64_t> GetUnsigned(const JSON* a_value)
        {
            // nlohmann::json keeps every non-negative integer as unsigned.
            if (!a_value || !a_value->is_number_unsigned())
            {
                return std::nullopt;
            }
            return a_value->get<std::uint64_t>();
        }

        bool HasContent(const JSON* a_value)
        {
            return a_value && !a_value->is_null() && !(a_value->is_structured() && a_value->empty());
        }

        // SKEE writes "Plugin.esp|0ABCDE": the form ID without the plugin's load order
        // bits, in hexadecimal.
        std::optional<Multiplayer::FormReference> ParseFormIdentifier(const JSON* a_value)
        {
            if (!a_value || !a_value->is_string())
            {
                return std::nullopt;
            }
            const auto& text = a_value->get_ref<const std::string&>();
            const auto separator = text.find('|');
            if (separator == std::string::npos || separator == 0)
            {
                return std::nullopt;
            }
            const std::string_view id{ text.data() + separator + 1, text.size() - separator - 1 };
            std::uint32_t localFormID = 0;
            const auto [end, error] = std::from_chars(id.data(), id.data() + id.size(), localFormID, 16);
            if (id.empty() || error != std::errc{} || end != id.data() + id.size())
            {
                return std::nullopt;
            }
            Multiplayer::FormReference reference{ .plugin = text.substr(0, separator), .localFormID = localFormID };
            if (!Multiplayer::IsValidFormReference(reference, false))
            {
                return std::nullopt;
            }
            return reference;
        }

        Multiplayer::AppearanceColor ToColor(std::uint64_t a_rgb)
        {
            return Multiplayer::AppearanceColor{ .red = static_cast<std::uint8_t>((a_rgb >> 16) & 0xFF),
                .green = static_cast<std::uint8_t>((a_rgb >> 8) & 0xFF),
                .blue = static_cast<std::uint8_t>(a_rgb & 0xFF) };
        }

        void ReadHeadParts(const JSON* a_parts, PresetFace& a_face)
        {
            if (!a_parts || !a_parts->is_array())
            {
                return;
            }
            // Each entry's "type" is its index in the player's head part array rather
            // than BGSHeadPart::type, and "formId" depends on the load order.
            for (const auto& part : *a_parts)
            {
                if (auto reference = ParseFormIdentifier(Find(part, "formIdentifier")))
                {
                    a_face.headParts.push_back(std::move(*reference));
                }
                else
                {
                    ++a_face.unreadableEntries;
                }
            }
        }

        void ReadActor(const JSON* a_actor, PresetFace& a_face)
        {
            if (!a_actor)
            {
                return;
            }
            if (const auto hairColor = GetUnsigned(Find(*a_actor, "hairColor")))
            {
                a_face.hairColor = ToColor(*hairColor);
            }
            if (const auto* texture = Find(*a_actor, "headTexture"); texture && texture->is_string()
                && !texture->get_ref<const std::string&>().empty())
            {
                if (auto reference = ParseFormIdentifier(texture))
                {
                    a_face.faceDetails = std::move(*reference);
                }
                else
                {
                    ++a_face.unreadableEntries;
                }
            }
        }

        void ReadMorphs(const JSON* a_morphs, PresetFace& a_face)
        {
            const auto* defaults = a_morphs ? Find(*a_morphs, "default") : nullptr;
            const auto* morphs = defaults ? Find(*defaults, "morphs") : nullptr;
            if (morphs && morphs->is_array())
            {
                const auto count = (std::min)(morphs->size(), a_face.faceMorphs.size());
                for (std::size_t i = 0; i < count; ++i)
                {
                    const auto& value = (*morphs)[i];
                    const auto morph = value.is_number() ? value.get<double>() : 0.0;
                    constexpr auto kLimit = static_cast<double>((std::numeric_limits<float>::max)());
                    a_face.faceMorphs[i] = std::isfinite(morph) ? static_cast<float>(std::clamp(morph, -kLimit, kLimit))
                                                                : 0.0F;
                }
            }
            const auto* presets = defaults ? Find(*defaults, "presets") : nullptr;
            if (presets && presets->is_array())
            {
                const auto count = (std::min)(presets->size(), a_face.faceParts.size());
                for (std::size_t i = 0; i < count; ++i)
                {
                    const auto& value = (*presets)[i];
                    if (!value.is_number_integer())
                    {
                        continue;
                    }
                    const auto part = value.is_number_unsigned()
                        ? static_cast<std::int64_t>((std::min)(value.get<std::uint64_t>(),
                              static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())))
                        : value.get<std::int64_t>();
                    a_face.faceParts[i] = part == kUnsetFacePartByte || part == kUnsetFacePartWord
                        ? Multiplayer::PlayerAppearance::kMinimumFacePart
                        : static_cast<std::int32_t>(std::clamp<std::int64_t>(part,
                              Multiplayer::PlayerAppearance::kMinimumFacePart,
                              Multiplayer::PlayerAppearance::kMaximumFacePart));
                }
            }
            a_face.raceMenuData = a_face.raceMenuData || (a_morphs && (HasContent(Find(*a_morphs, "custom"))
                                                                          || HasContent(Find(*a_morphs, "sculpt"))));
        }

        void ReadTints(const JSON* a_tints, PresetFace& a_face)
        {
            if (!a_tints || !a_tints->is_array())
            {
                return;
            }
            // "index" is the layer's place in the player's tint mask list. "color" is
            // ARGB, with the mask's alpha in the top byte.
            for (const auto& tint : *a_tints)
            {
                const auto color = GetUnsigned(Find(tint, "color"));
                const auto* texture = Find(tint, "texture");
                if (!color || *color > 0xFFFFFFFF || !texture || !texture->is_string())
                {
                    ++a_face.unreadableEntries;
                    continue;
                }
                const auto alpha = static_cast<float>((*color >> 24) & 0xFF) / 255.0F;
                if (alpha > 0.0F)
                {
                    a_face.tints.push_back(PresetTint{
                        .texture = texture->get<std::string>(), .color = ToColor(*color), .alpha = alpha });
                }
            }
        }
    }

    std::vector<std::string> List()
    {
        std::vector<std::string> names;
        std::error_code error;
        for (std::filesystem::directory_iterator entry{ ToPath(kDirectory), error }, end; !error && entry != end;
            entry.increment(error))
        {
            if (!entry->is_regular_file(error)
                || !ASCIICase::Equals(PluginFiles::ToUTF8(entry->path().extension()), kExtension))
            {
                continue;
            }
            auto name = PluginFiles::ToUTF8(entry->path().stem());
            if (Config::DefaultLookSettings::IsValidPresetName(name))
            {
                names.push_back(std::move(name));
            }
        }
        std::ranges::sort(names, [](const std::string& a_left, const std::string& a_right) {
            return ASCIICase::Compare(a_left, a_right) < 0;
        });
        return names;
    }

    std::expected<PresetFace, std::string> Load(std::string_view a_name)
    {
        if (!Config::DefaultLookSettings::IsValidPresetName(a_name))
        {
            return std::unexpected("invalid name");
        }
        const auto path = ToPath(kDirectory) / ToPath(std::format("{}{}", a_name, kExtension));
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error)
        {
            return std::unexpected(std::format("file: {}", error.message()));
        }
        if (size > kMaximumFileSize)
        {
            return std::unexpected(std::format("file size {}", size));
        }
        const auto text = PluginFiles::ReadFile(path);
        if (!text)
        {
            return std::unexpected("file unreadable");
        }
        return Parse(*text);
    }

    std::expected<PresetFace, std::string> Parse(std::string_view a_text)
    {
        const auto root = JSON::parse(a_text, nullptr, false);
        if (root.is_discarded() || !root.is_object())
        {
            return std::unexpected("not JSON");
        }
        const auto* version = Find(root, "version");
        const auto signature = GetUnsigned(version ? Find(*version, "signature") : nullptr);
        const auto formatVersion = GetUnsigned(version ? Find(*version, "formatVersion") : nullptr);
        if (signature != kSignature || !formatVersion || *formatVersion == 0)
        {
            return std::unexpected("not a RaceMenu preset");
        }

        PresetFace face;
        ReadHeadParts(Find(root, "headParts"), face);
        ReadActor(Find(root, "actor"), face);
        ReadMorphs(Find(root, "morphs"), face);
        ReadTints(Find(root, "tintInfo"), face);
        // Not counted: SKEE fills faceTextures from the face part's texture set even when
        // nothing replaced it.
        for (const auto key : { "transforms", "overrides", "skinOverrides", "bodyMorphs" })
        {
            face.raceMenuData = face.raceMenuData || HasContent(Find(root, key));
        }
        return face;
    }
}
