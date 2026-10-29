#pragma once

#include "protocol/FormReference.h"
#include "protocol/PeerSlot.h"
#include "text/AsciiCase.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Multiplayer
{
    using AppearanceFormReference = FormReference;

    struct AppearanceColor
    {
        std::uint8_t red{ 0 };
        std::uint8_t green{ 0 };
        std::uint8_t blue{ 0 };

        constexpr bool operator==(const AppearanceColor&) const noexcept = default;
    };

    struct AppearanceHeadPart
    {
        AppearanceFormReference form;
        // BGSHeadPart::type (PNAM). Mods use values beyond the vanilla HeadPartType range.
        std::uint32_t type{ 0 };

        bool operator==(const AppearanceHeadPart&) const = default;
    };

    struct AppearanceTint
    {
        // TintAsset::TintLayer::index of the source race and sex.
        std::uint16_t index{ 0 };
        // TintMask::Type, which uses the same numbering as TintLayer::SkinTone.
        std::uint8_t type{ 0 };
        AppearanceColor color{};
        float alpha{ 0.0F };
        // TintMask::texture's path, relative to Data\Textures. Empty from 1.1.0 senders.
        std::string texture;

        bool operator==(const AppearanceTint&) const = default;
    };

    enum class OverlayArea : std::uint8_t
    {
        kBody = 1,
        kHands = 2,
        kFeet = 3,
        kFace = 4,
    };

    struct AppearanceOverlay
    {
        OverlayArea area{ OverlayArea::kBody };
        std::string texture;
        AppearanceColor color{};
        float alpha{ 0.0F };
        AppearanceColor glowColor{};
        float glowStrength{ 0.0F };

        bool operator==(const AppearanceOverlay&) const = default;
    };

    struct PlayerAppearance
    {
        static constexpr std::size_t kFaceMorphCount = 19;
        static constexpr std::size_t kFacePartCount = 4;
        static constexpr std::size_t kMaximumHeadParts = 32;
        static constexpr std::size_t kMaximumTints = 64;
        static constexpr std::size_t kMaximumOverlays = 32;
        static constexpr std::size_t kMaximumTexturePathLength = 260;
        static constexpr float kMaximumOverlayGlowStrength = 100.0F;
        static constexpr std::size_t kMaximumNameLength = 128;
        static constexpr std::size_t kMaximumPluginNameLength = FormReference::kMaximumPluginNameLength;
        static constexpr std::uint32_t kMaximumLocalFormID = FormReference::kMaximumLocalFormID;
        static constexpr std::uint8_t kMaximumTintType = 14;
        static constexpr float kMaximumWeight = 100.0F;
        static constexpr float kMinimumHeight = 0.25F;
        static constexpr float kMaximumHeight = 4.0F;
        // -1 is the engine's unset part. The race preset bitmasks hold 256 bits per
        // part type (TESRace +0x4A8), so no preset index exceeds 255.
        static constexpr std::int32_t kMinimumFacePart = -1;
        static constexpr std::int32_t kMaximumFacePart = 255;

        std::string name;
        std::uint16_t level{ 0 };
        AppearanceFormReference race;
        bool female{ false };
        float weight{ 50.0F };
        float height{ 1.0F };
        std::vector<AppearanceHeadPart> headParts;
        std::optional<AppearanceColor> hairColor;
        AppearanceFormReference faceDetails;
        AppearanceColor bodyTint{};
        std::array<float, kFaceMorphCount> faceMorphs{};
        std::array<std::int32_t, kFacePartCount> faceParts{};
        std::vector<AppearanceTint> tints;
        std::vector<AppearanceOverlay> overlays;
        std::optional<AppearanceFormReference> defaultRace;
        std::uint64_t revision{ 0 };

        [[nodiscard]] bool HasSameLook(const PlayerAppearance& a_other) const
        {
            return race == a_other.race && female == a_other.female && weight == a_other.weight
                && height == a_other.height && headParts == a_other.headParts && hairColor == a_other.hairColor
                && faceDetails == a_other.faceDetails && bodyTint == a_other.bodyTint
                && faceMorphs == a_other.faceMorphs && faceParts == a_other.faceParts && tints == a_other.tints
                && overlays == a_other.overlays && defaultRace == a_other.defaultRace;
        }

        [[nodiscard]] bool HasSameLabel(const PlayerAppearance& a_other) const
        {
            return name == a_other.name && level == a_other.level;
        }
    };

    [[nodiscard]] constexpr std::int32_t ClampFacePart(std::int32_t a_part) noexcept
    {
        return std::clamp(a_part, PlayerAppearance::kMinimumFacePart, PlayerAppearance::kMaximumFacePart);
    }

    struct PlayerInspection
    {
        PeerSlot playerID{ 0 };
        std::uint16_t level{ 0 };
        std::optional<AppearanceFormReference> race;
        std::optional<AppearanceFormReference> defaultRace;
    };

    [[nodiscard]] inline bool IsValidAppearanceName(std::string_view a_name)
    {
        if (a_name.size() > PlayerAppearance::kMaximumNameLength)
        {
            return false;
        }
        for (const auto character : a_name)
        {
            const auto byte = static_cast<unsigned char>(character);
            if (byte < 0x20 || byte == 0x7F)
            {
                return false;
            }
        }
        return true;
    }

    // A relative path to a .dds file in printable ASCII that cannot leave the data folder.
    [[nodiscard]] constexpr bool IsValidTexturePath(std::string_view a_path) noexcept
    {
        if (a_path.empty() || a_path.size() > PlayerAppearance::kMaximumTexturePathLength
            || !AsciiCase::EndsWith(a_path, ".dds") || a_path.front() == '\\' || a_path.front() == '/')
        {
            return false;
        }
        std::size_t componentStart = 0;
        for (std::size_t i = 0; i <= a_path.size(); ++i)
        {
            if (i == a_path.size() || a_path[i] == '\\' || a_path[i] == '/')
            {
                if (a_path.substr(componentStart, i - componentStart) == "..")
                {
                    return false;
                }
                componentStart = i + 1;
                continue;
            }
            const auto byte = static_cast<unsigned char>(a_path[i]);
            if (byte < 0x20 || byte > 0x7E || a_path[i] == ':')
            {
                return false;
            }
        }
        return true;
    }

    static_assert(IsValidTexturePath("Actors\\Character\\Character Assets\\TintMasks\\BeautyMark_01.DDS"));
    static_assert(!IsValidTexturePath("..\\x.dds") && !IsValidTexturePath("a/../x.dds") && !IsValidTexturePath("C:x.dds"));
    static_assert(!IsValidTexturePath("\\x.dds") && !IsValidTexturePath("x.png") && !IsValidTexturePath(""));

    [[nodiscard]] inline bool IsValidAppearance(const PlayerAppearance& a_appearance)
    {
        if (a_appearance.revision == 0 || !IsValidAppearanceName(a_appearance.name)
            || a_appearance.revision > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
            || !IsValidFormReference(a_appearance.race, false) || !IsValidFormReference(a_appearance.faceDetails, true)
            || !std::isfinite(a_appearance.weight) || a_appearance.weight < 0.0F
            || a_appearance.weight > PlayerAppearance::kMaximumWeight || !std::isfinite(a_appearance.height)
            || a_appearance.height < PlayerAppearance::kMinimumHeight
            || a_appearance.height > PlayerAppearance::kMaximumHeight
            || a_appearance.headParts.size() > PlayerAppearance::kMaximumHeadParts
            || a_appearance.tints.size() > PlayerAppearance::kMaximumTints
            || a_appearance.overlays.size() > PlayerAppearance::kMaximumOverlays)
        {
            return false;
        }
        if (a_appearance.defaultRace
            && (!IsValidFormReference(*a_appearance.defaultRace, false)
                || !IsBaseGamePlugin(a_appearance.defaultRace->plugin)))
        {
            return false;
        }

        for (const auto& headPart : a_appearance.headParts)
        {
            if (!IsValidFormReference(headPart.form, false))
            {
                return false;
            }
        }
        for (const auto morph : a_appearance.faceMorphs)
        {
            if (!std::isfinite(morph))
            {
                return false;
            }
        }
        for (const auto& tint : a_appearance.tints)
        {
            if (tint.type > PlayerAppearance::kMaximumTintType || !std::isfinite(tint.alpha) || tint.alpha < 0.0F
                || tint.alpha > 1.0F || (!tint.texture.empty() && !IsValidTexturePath(tint.texture)))
            {
                return false;
            }
        }
        for (const auto& overlay : a_appearance.overlays)
        {
            if (overlay.area < OverlayArea::kBody || overlay.area > OverlayArea::kFace
                || !IsValidTexturePath(overlay.texture) || !std::isfinite(overlay.alpha) || overlay.alpha < 0.0F
                || overlay.alpha > 1.0F || !std::isfinite(overlay.glowStrength) || overlay.glowStrength < 0.0F
                || overlay.glowStrength > PlayerAppearance::kMaximumOverlayGlowStrength)
            {
                return false;
            }
        }
        return true;
    }
}
