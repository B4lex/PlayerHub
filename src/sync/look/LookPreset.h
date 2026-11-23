#pragma once

#include "protocol/AppearanceState.h"
#include "protocol/FormReference.h"

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace LookPreset
{
    struct PresetTint
    {
        // TintMask::texture's path, relative to Data\Textures.
        std::string texture;
        Multiplayer::AppearanceColor color{};
        float alpha{ 0.0F };
    };

    // A RaceMenu preset's vanilla face data, before it is resolved against a race.
    struct PresetFace
    {
        std::vector<Multiplayer::FormReference> headParts;
        std::optional<Multiplayer::AppearanceColor> hairColor;
        Multiplayer::FormReference faceDetails;
        std::array<float, Multiplayer::AppearanceDefaultFace::kFaceMorphCount> faceMorphs{};
        std::array<std::int32_t, Multiplayer::AppearanceDefaultFace::kFacePartCount> faceParts{ -1, -1, -1, -1 };
        // Visible layers only.
        std::vector<PresetTint> tints;
        // Head part and tint entries whose fields could not be read.
        std::uint32_t unreadableEntries{ 0 };
        // Custom slider morphs, sculpting, transforms, overrides, or body morphs.
        bool raceMenuData{ false };
    };

    // File stems of Data\SKSE\Plugins\CharGen\Presets\*.jslot, sorted.
    [[nodiscard]] std::vector<std::string> List();

    // On failure, the reason for the log.
    [[nodiscard]] std::expected<PresetFace, std::string> Load(std::string_view a_name);

    [[nodiscard]] std::expected<PresetFace, std::string> Parse(std::string_view a_text);
}
