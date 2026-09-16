#pragma once

#include "FormReference.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
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
		// BGSHeadPart::HeadPartType. Kept so an unresolved part can fall back to
		// the race default of the same type.
		std::uint8_t type{ 0 };

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

		bool operator==(const AppearanceTint&) const = default;
	};

	// Reconstructable character appearance. Unlike the transform checkpoint it
	// changes rarely, so it is published as its own latest-wins state.
	struct PlayerAppearance
	{
		static constexpr std::size_t kFaceMorphCount = 19;
		static constexpr std::size_t kFacePartCount = 4;
		static constexpr std::size_t kMaximumHeadParts = 16;
		static constexpr std::size_t kMaximumTints = 64;
		// UTF-8 bytes, not characters.
		static constexpr std::size_t kMaximumNameLength = 128;
		static constexpr std::size_t kMaximumPluginNameLength =
			FormReference::kMaximumPluginNameLength;
		static constexpr std::uint32_t kMaximumLocalFormID =
			FormReference::kMaximumLocalFormID;
		static constexpr std::uint8_t kMaximumHeadPartType = 6;
		static constexpr std::uint8_t kMaximumTintType = 14;
		static constexpr float kMaximumWeight = 100.0F;
		static constexpr float kMinimumHeight = 0.25F;
		static constexpr float kMaximumHeight = 4.0F;

		// Character name shown on the puppet. It is not part of the look, but it
		// lives on the same actor base and changes about as rarely.
		std::string name;
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
		// Source-local publication counter. Not part of the look itself.
		std::uint64_t revision{ 0 };

		[[nodiscard]] bool HasSameLook(const PlayerAppearance& a_other) const
		{
			return name == a_other.name &&
				race == a_other.race &&
				female == a_other.female &&
				weight == a_other.weight &&
				height == a_other.height &&
				headParts == a_other.headParts &&
				hairColor == a_other.hairColor &&
				faceDetails == a_other.faceDetails &&
				bodyTint == a_other.bodyTint &&
				faceMorphs == a_other.faceMorphs &&
				faceParts == a_other.faceParts &&
				tints == a_other.tints;
		}
	};

	// Control characters are rejected, and so are overlong names. Multibyte UTF-8
	// sequences are passed through unchanged.
	[[nodiscard]] inline bool IsValidAppearanceName(const std::string& a_name)
	{
		if (a_name.size() > PlayerAppearance::kMaximumNameLength) {
			return false;
		}
		for (const auto character : a_name) {
			const auto byte = static_cast<unsigned char>(character);
			if (byte < 0x20 || byte == 0x7F) {
				return false;
			}
		}
		return true;
	}

	// Shared by capture and by the network parser so both sides enforce the same
	// bounds as the server.
	[[nodiscard]] inline bool IsValidAppearance(const PlayerAppearance& a_appearance)
	{
		if (a_appearance.revision == 0 ||
			!IsValidAppearanceName(a_appearance.name) ||
			a_appearance.revision > static_cast<std::uint64_t>(
				(std::numeric_limits<std::int64_t>::max)()) ||
			!IsValidFormReference(a_appearance.race, false) ||
			!IsValidFormReference(a_appearance.faceDetails, true) ||
			!std::isfinite(a_appearance.weight) ||
			a_appearance.weight < 0.0F ||
			a_appearance.weight > PlayerAppearance::kMaximumWeight ||
			!std::isfinite(a_appearance.height) ||
			a_appearance.height < PlayerAppearance::kMinimumHeight ||
			a_appearance.height > PlayerAppearance::kMaximumHeight ||
			a_appearance.headParts.size() > PlayerAppearance::kMaximumHeadParts ||
			a_appearance.tints.size() > PlayerAppearance::kMaximumTints) {
			return false;
		}

		for (const auto& headPart : a_appearance.headParts) {
			if (!IsValidFormReference(headPart.form, false) ||
				headPart.type > PlayerAppearance::kMaximumHeadPartType) {
				return false;
			}
		}
		for (const auto morph : a_appearance.faceMorphs) {
			if (!std::isfinite(morph)) {
				return false;
			}
		}
		for (const auto& tint : a_appearance.tints) {
			if (tint.type > PlayerAppearance::kMaximumTintType ||
				!std::isfinite(tint.alpha) ||
				tint.alpha < 0.0F ||
				tint.alpha > 1.0F) {
				return false;
			}
		}
		return true;
	}
}
