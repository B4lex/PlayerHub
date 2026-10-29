#include "sync/appearance/AppearanceSync.h"
#include "actor/RaceMenuOverlays.h"
#include "forms/BeastForms.h"
#include "forms/DynamicForm.h"
#include "forms/FormLookup.h"
#include "forms/RaceForms.h"
#include "forms/TextureFiles.h"
#include "network/ServerClient.h"
#include "runtime/FrameTime.h"
#include "runtime/Profiling.h"
#include "sync/PlayerLabel.h"
#include "sync/outfit/DefaultOutfit.h"
#include "text/AsciiCase.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AppearanceSync
{
    namespace
    {
        constexpr float kCaptureIntervalSeconds = 1.0F;
        constexpr float kMinimumPublishIntervalSeconds = 3.0F;
        constexpr float kUnsettledCaptureWarningSeconds = 10.0F;
        constexpr float kFirstAppearanceWaitSeconds = 3.0F;
        constexpr float kMinimumRespawnIntervalSeconds = 2.0F;
        // Bounds respawns from a form that flips back and forth; vanilla transformations
        // take far longer.
        constexpr float kMinimumShapeshiftIntervalSeconds = 1.0F;
        constexpr float kMinimumRemoteHeight = 0.85F;
        constexpr float kMaximumRemoteHeight = 1.15F;
        constexpr std::string_view kFallbackRacePlugin = "Skyrim.esm";
        constexpr RE::FormID kFallbackRaceLocalFormID = 0x00013746; // NordRace

        using Multiplayer::AppearanceColor;
        using Multiplayer::AppearanceFormReference;
        using Multiplayer::AppearanceHeadPart;
        using Multiplayer::AppearanceOverlay;
        using Multiplayer::AppearanceTint;
        using AppearanceData = Multiplayer::PlayerAppearance;
        using FormLookup::MakeFormReference;
        using FormLookup::ResolveForm;
        using Forms::IsPlayableRace;
        using HeadPartType = RE::BGSHeadPart::HeadPartType;
        using FaceRelatedData = RE::TESRace::FaceRelatedData;
        using TintAsset = FaceRelatedData::TintAsset;

        constexpr AppearanceColor ToAppearanceColor(const RE::Color& a_color) noexcept
        {
            return AppearanceColor{ .red = a_color.red, .green = a_color.green, .blue = a_color.blue };
        }

        FaceRelatedData* GetFaceRelatedData(RE::TESRace* a_race, bool a_female)
        {
            return a_race ? a_race->faceRelatedData[a_female ? RE::SEXES::kFemale : RE::SEXES::kMale] : nullptr;
        }

        const TintAsset* FindTintAsset(const FaceRelatedData* a_faceData, const RE::TintMask& a_mask)
        {
            if (!a_faceData || !a_faceData->tintMasks || !a_mask.texture)
            {
                return nullptr;
            }

            const std::string_view maskPath{ a_mask.texture->textureName.c_str() };
            for (const auto* asset : *a_faceData->tintMasks)
            {
                if (asset && &asset->texture.file == a_mask.texture)
                {
                    return asset;
                }
            }
            if (maskPath.empty())
            {
                return nullptr;
            }
            for (const auto* asset : *a_faceData->tintMasks)
            {
                if (asset && AsciiCase::Equals(asset->texture.file.textureName.c_str(), maskPath))
                {
                    return asset;
                }
            }
            return nullptr;
        }

        const TintAsset* FindTintAsset(const FaceRelatedData* a_faceData, std::uint16_t a_index, std::uint8_t a_type)
        {
            if (!a_faceData || !a_faceData->tintMasks)
            {
                return nullptr;
            }

            for (const auto* asset : *a_faceData->tintMasks)
            {
                if (asset && asset->texture.index == a_index && asset->texture.skinTone.underlying() == a_type)
                {
                    return asset;
                }
            }
            return nullptr;
        }

        const TintAsset* FindTintAsset(const FaceRelatedData* a_faceData, std::uint16_t a_index)
        {
            if (!a_faceData || !a_faceData->tintMasks)
            {
                return nullptr;
            }

            for (const auto* asset : *a_faceData->tintMasks)
            {
                if (asset && asset->texture.index == a_index)
                {
                    return asset;
                }
            }
            return nullptr;
        }

        void LogCaptureFailure(std::string_view a_reason)
        {
            static std::string lastReason;
            if (lastReason == a_reason)
            {
                return;
            }
            lastReason = a_reason;
            logs::warn("Appearance sync could not capture local appearance: {}", a_reason);
        }

        void LogInvalidTintTexture(const std::string& a_texture)
        {
            static std::string lastTexture;
            if (lastTexture == a_texture)
            {
                return;
            }
            lastTexture = a_texture;
            logs::warn("Appearance sync left out a tint whose texture path peers would reject: texture=\"{}\"", a_texture);
        }

        // Called once per capture with the first head part left out, or none.
        void LogHeadPartOverflow(const std::optional<AppearanceFormReference>& a_firstLeftOut)
        {
            static bool overflowing = false;
            if (overflowing == a_firstLeftOut.has_value())
            {
                return;
            }
            overflowing = a_firstLeftOut.has_value();
            if (overflowing)
            {
                logs::warn("Appearance sync left out head parts over the limit: limit={} firstLeftOut={}|{:06X}",
                    AppearanceData::kMaximumHeadParts, a_firstLeftOut->plugin, a_firstLeftOut->localFormID);
            }
        }

        template <class T>
        std::size_t FindFirstDifference(const T& a_previous, const T& a_current)
        {
            const auto [previous, current] = std::ranges::mismatch(a_previous, a_current);
            return static_cast<std::size_t>(std::ranges::distance(std::ranges::begin(a_previous), previous));
        }

        std::string FormatTint(const std::vector<AppearanceTint>& a_tints, std::size_t a_index)
        {
            if (a_index >= a_tints.size())
            {
                return "none";
            }
            const auto& tint = a_tints[a_index];
            return std::format("{}:{} #{:02X}{:02X}{:02X} alpha={} texture=\"{}\"", tint.index, tint.type,
                tint.color.red, tint.color.green, tint.color.blue, tint.alpha, tint.texture);
        }

        std::string FormatOverlay(const std::vector<AppearanceOverlay>& a_overlays, std::size_t a_index)
        {
            if (a_index >= a_overlays.size())
            {
                return "none";
            }
            const auto& overlay = a_overlays[a_index];
            return std::format("area={} #{:02X}{:02X}{:02X} alpha={} glow=#{:02X}{:02X}{:02X}x{} texture=\"{}\"",
                static_cast<int>(overlay.area), overlay.color.red, overlay.color.green, overlay.color.blue,
                overlay.alpha, overlay.glowColor.red, overlay.glowColor.green, overlay.glowColor.blue,
                overlay.glowStrength, overlay.texture);
        }

        std::string FormatHeadPart(const std::vector<AppearanceHeadPart>& a_headParts, std::size_t a_index)
        {
            if (a_index >= a_headParts.size())
            {
                return "none";
            }
            const auto& headPart = a_headParts[a_index];
            return std::format("{}|{:06X}", headPart.form.plugin, headPart.form.localFormID);
        }

        std::string DescribeCaptureChange(const AppearanceData& a_previous, const AppearanceData& a_current)
        {
            std::string changes;
            const auto add = [&changes](std::string_view a_change) {
                if (!changes.empty())
                {
                    changes += "; ";
                }
                changes += a_change;
            };
            if (a_previous.race != a_current.race || a_previous.female != a_current.female)
            {
                add(std::format("race {}|{:06X} female={} -> {}|{:06X} female={}", a_previous.race.plugin,
                    a_previous.race.localFormID, a_previous.female, a_current.race.plugin, a_current.race.localFormID,
                    a_current.female));
            }
            if (a_previous.weight != a_current.weight || a_previous.height != a_current.height)
            {
                add(std::format("weight {} -> {} height {} -> {}", a_previous.weight, a_current.weight,
                    a_previous.height, a_current.height));
            }
            if (a_previous.headParts != a_current.headParts)
            {
                const auto index = FindFirstDifference(a_previous.headParts, a_current.headParts);
                add(std::format("headParts count {} -> {} first [{}] {} -> {}", a_previous.headParts.size(),
                    a_current.headParts.size(), index, FormatHeadPart(a_previous.headParts, index),
                    FormatHeadPart(a_current.headParts, index)));
            }
            if (a_previous.hairColor != a_current.hairColor)
            {
                add("hairColor");
            }
            if (a_previous.faceDetails != a_current.faceDetails)
            {
                add(std::format("faceDetails {}|{:06X} -> {}|{:06X}", a_previous.faceDetails.plugin,
                    a_previous.faceDetails.localFormID, a_current.faceDetails.plugin,
                    a_current.faceDetails.localFormID));
            }
            if (a_previous.bodyTint != a_current.bodyTint)
            {
                add("bodyTint");
            }
            if (a_previous.faceMorphs != a_current.faceMorphs)
            {
                const auto index = FindFirstDifference(a_previous.faceMorphs, a_current.faceMorphs);
                add(std::format(
                    "faceMorphs[{}] {} -> {}", index, a_previous.faceMorphs[index], a_current.faceMorphs[index]));
            }
            if (a_previous.faceParts != a_current.faceParts)
            {
                const auto index = FindFirstDifference(a_previous.faceParts, a_current.faceParts);
                add(std::format(
                    "faceParts[{}] {} -> {}", index, a_previous.faceParts[index], a_current.faceParts[index]));
            }
            if (a_previous.tints != a_current.tints)
            {
                const auto index = FindFirstDifference(a_previous.tints, a_current.tints);
                add(std::format("tints count {} -> {} first [{}] {} -> {}", a_previous.tints.size(),
                    a_current.tints.size(), index, FormatTint(a_previous.tints, index),
                    FormatTint(a_current.tints, index)));
            }
            if (a_previous.overlays != a_current.overlays)
            {
                const auto index = FindFirstDifference(a_previous.overlays, a_current.overlays);
                add(std::format("overlays count {} -> {} first [{}] {} -> {}", a_previous.overlays.size(),
                    a_current.overlays.size(), index, FormatOverlay(a_previous.overlays, index),
                    FormatOverlay(a_current.overlays, index)));
            }
            if (a_previous.defaultRace != a_current.defaultRace)
            {
                add("defaultRace");
            }
            if (a_previous.level != a_current.level)
            {
                add(std::format("level {} -> {}", a_previous.level, a_current.level));
            }
            return changes;
        }

        // Follows Multiplayer::IsValidAppearance check for check and names the first that fails.
        std::string DescribeInvalidAppearance(const AppearanceData& a_appearance)
        {
            using Multiplayer::IsValidFormReference;
            const auto formatForm = [](const AppearanceFormReference& a_form) {
                return std::format("{}|{:06X}", a_form.plugin, a_form.localFormID);
            };
            const auto inRange = [](float a_value, float a_minimum, float a_maximum) {
                return std::isfinite(a_value) && a_value >= a_minimum && a_value <= a_maximum;
            };

            if (!Multiplayer::IsValidAppearanceName(a_appearance.name))
            {
                return std::format("name length={}", a_appearance.name.size());
            }
            if (!IsValidFormReference(a_appearance.race, false))
            {
                return std::format("race {}", formatForm(a_appearance.race));
            }
            if (!IsValidFormReference(a_appearance.faceDetails, true))
            {
                return std::format("faceDetails {}", formatForm(a_appearance.faceDetails));
            }
            if (!inRange(a_appearance.weight, 0.0F, AppearanceData::kMaximumWeight))
            {
                return std::format("weight {}", a_appearance.weight);
            }
            if (!inRange(a_appearance.height, AppearanceData::kMinimumHeight, AppearanceData::kMaximumHeight))
            {
                return std::format("height {}", a_appearance.height);
            }
            if (a_appearance.headParts.size() > AppearanceData::kMaximumHeadParts)
            {
                return std::format("headParts count {}", a_appearance.headParts.size());
            }
            if (a_appearance.tints.size() > AppearanceData::kMaximumTints)
            {
                return std::format("tints count {}", a_appearance.tints.size());
            }
            if (a_appearance.overlays.size() > AppearanceData::kMaximumOverlays)
            {
                return std::format("overlays count {}", a_appearance.overlays.size());
            }
            if (const auto& defaultRace = a_appearance.defaultRace; defaultRace
                && (!IsValidFormReference(*defaultRace, false) || !Multiplayer::IsBaseGamePlugin(defaultRace->plugin)))
            {
                return std::format("defaultRace {}", formatForm(*defaultRace));
            }
            for (std::size_t i = 0; i < a_appearance.headParts.size(); ++i)
            {
                const auto& headPart = a_appearance.headParts[i];
                if (!IsValidFormReference(headPart.form, false))
                {
                    return std::format(
                        "headParts[{}] {} type={}", i, FormatHeadPart(a_appearance.headParts, i), headPart.type);
                }
            }
            for (std::size_t i = 0; i < a_appearance.faceMorphs.size(); ++i)
            {
                if (!std::isfinite(a_appearance.faceMorphs[i]))
                {
                    return std::format("faceMorphs[{}] {}", i, a_appearance.faceMorphs[i]);
                }
            }
            for (std::size_t i = 0; i < a_appearance.tints.size(); ++i)
            {
                const auto& tint = a_appearance.tints[i];
                if (tint.type > AppearanceData::kMaximumTintType || !inRange(tint.alpha, 0.0F, 1.0F)
                    || (!tint.texture.empty() && !Multiplayer::IsValidTexturePath(tint.texture)))
                {
                    return std::format("tints[{}] {}", i, FormatTint(a_appearance.tints, i));
                }
            }
            for (std::size_t i = 0; i < a_appearance.overlays.size(); ++i)
            {
                const auto& overlay = a_appearance.overlays[i];
                if (overlay.area < Multiplayer::OverlayArea::kBody || overlay.area > Multiplayer::OverlayArea::kFace
                    || !Multiplayer::IsValidTexturePath(overlay.texture) || !inRange(overlay.alpha, 0.0F, 1.0F)
                    || !inRange(overlay.glowStrength, 0.0F, AppearanceData::kMaximumOverlayGlowStrength))
                {
                    return std::format("overlays[{}] {}", i, FormatOverlay(a_appearance.overlays, i));
                }
            }
            return std::format("revision {}", a_appearance.revision);
        }

        float GetSkeletonHeightScale(RE::PlayerCharacter& a_player)
        {
            static float lastScale = 1.0F;
            auto* root = a_player.Get3D(false);
            if (!root)
            {
                return lastScale;
            }
            static const RE::BSFixedString nodeName{ "NPC" };
            const auto* node = root->GetObjectByName(nodeName);
            const float nodeScale = node ? node->local.scale : 1.0F;
            // Rounded so float noise in a recomputed transform does not publish a revision.
            const float scale
                = std::isfinite(nodeScale) && nodeScale > 0.0F ? std::round(nodeScale * 100.0F) / 100.0F : 1.0F;
            if (scale != lastScale)
            {
                logs::debug("Appearance sync skeleton height scale changed: {} -> {} node={}", lastScale, scale,
                    node != nullptr);
                lastScale = scale;
            }
            return scale;
        }

        std::optional<AppearanceData> CaptureLocalAppearance(RE::PlayerCharacter& a_player)
        {
            auto* base = a_player.GetActorBase();
            if (!base)
            {
                LogCaptureFailure("player has no actor base");
                return std::nullopt;
            }
            // Transformation races replace the skeleton and behavior graph. The player's
            // originalRace is the player record's template race, so it comes last.
            auto* race = Forms::GetPlayableRace(base->race);
            if (!race)
            {
                race = IsPlayableRace(base->originalRace) ? base->originalRace : nullptr;
            }
            const auto raceReference = MakeFormReference(race);
            if (!raceReference)
            {
                const auto* file = race ? race->GetFile(0) : nullptr;
                LogCaptureFailure(
                    std::format("race is not referenceable: race={:08X} playable={} originalRace={:08X} "
                                "originalPlayable={} dynamic={} file={}",
                        base->race ? base->race->GetFormID() : 0, IsPlayableRace(base->race),
                        base->originalRace ? base->originalRace->GetFormID() : 0, IsPlayableRace(base->originalRace),
                        race && race->IsDynamicForm(), file ? file->GetFilename() : std::string_view{ "<none>" }));
                return std::nullopt;
            }

            const float height = base->height * (a_player.GetReferenceRuntimeData().refScale / 100.0F)
                * GetSkeletonHeightScale(a_player);
            // The server names the player after its session.
            AppearanceData appearance{ .level = a_player.GetLevel(),
                .race = *raceReference,
                .female = base->IsFemale(),
                .weight = std::clamp(base->weight, 0.0F, AppearanceData::kMaximumWeight),
                .height = std::clamp(height, AppearanceData::kMinimumHeight, AppearanceData::kMaximumHeight) };

            std::optional<AppearanceFormReference> firstLeftOut;
            if (base->headParts)
            {
                for (std::int8_t i = 0; i < base->numHeadParts; ++i)
                {
                    auto* headPart = base->headParts[i];
                    // Extra parts are children of their top-level part and are implied.
                    if (!headPart || headPart->IsExtraPart())
                    {
                        continue;
                    }
                    const auto reference = MakeFormReference(headPart);
                    if (!reference)
                    {
                        continue;
                    }
                    if (appearance.headParts.size() >= AppearanceData::kMaximumHeadParts)
                    {
                        if (!firstLeftOut)
                        {
                            firstLeftOut = reference;
                        }
                        continue;
                    }
                    appearance.headParts.push_back(
                        AppearanceHeadPart{ .form = *reference, .type = headPart->type.underlying() });
                }
            }
            LogHeadPartOverflow(firstLeftOut);

            if (base->headRelatedData)
            {
                if (const auto* hairColor = base->headRelatedData->hairColor)
                {
                    appearance.hairColor = ToAppearanceColor(hairColor->color);
                }
                if (const auto faceDetails = MakeFormReference(base->headRelatedData->faceDetails))
                {
                    appearance.faceDetails = *faceDetails;
                }
            }

            if (base->faceData)
            {
                for (std::size_t i = 0; i < AppearanceData::kFaceMorphCount; ++i)
                {
                    const auto morph = base->faceData->morphs[i];
                    appearance.faceMorphs[i] = std::isfinite(morph) ? morph : 0.0F;
                }
                for (std::size_t i = 0; i < AppearanceData::kFacePartCount; ++i)
                {
                    appearance.faceParts[i] = Multiplayer::ClampFacePart(base->faceData->parts[i]);
                }
            }

            appearance.bodyTint = ToAppearanceColor(base->bodyTintColor);
            // The player's face tints live on PlayerCharacter rather than TESNPC. The
            // runtime-data layout is only known for flat Skyrim.
            if (!REL::Module::IsVR())
            {
                const auto* faceData = GetFaceRelatedData(race, appearance.female);
                for (const auto* mask : a_player.GetPlayerRuntimeData().tintMasks)
                {
                    if (!mask)
                    {
                        continue;
                    }
                    const auto type = static_cast<std::uint8_t>(mask->type.underlying());
                    if (type == static_cast<std::uint8_t>(RE::TintMask::Type::kSkinTone)
                        && appearance.bodyTint == AppearanceColor{})
                    {
                        appearance.bodyTint = ToAppearanceColor(mask->color);
                    }
                    if (!std::isfinite(mask->alpha) || mask->alpha <= 0.0F || type > AppearanceData::kMaximumTintType
                        || appearance.tints.size() >= AppearanceData::kMaximumTints)
                    {
                        continue;
                    }
                    const auto* asset = FindTintAsset(faceData, *mask);
                    if (!asset)
                    {
                        continue;
                    }
                    // RaceMenu's alternative and add-on war paints rewrite the race asset's
                    // texture path (Game.SetTintMaskTexturePath), so the mask's path is the
                    // one shown, and peers' race assets still hold the original.
                    const char* path = mask->texture->textureName.c_str();
                    std::string texture{ path ? path : "" };
                    if (!Multiplayer::IsValidTexturePath(texture))
                    {
                        LogInvalidTintTexture(texture);
                        continue;
                    }
                    appearance.tints.push_back(AppearanceTint{ .index = asset->texture.index,
                        .type = type,
                        .color = ToAppearanceColor(mask->color),
                        .alpha = (std::min)(mask->alpha, 1.0F),
                        .texture = std::move(texture) });
                }
            }
            appearance.overlays = RaceMenuOverlays::Capture(a_player);

            DefaultOutfit::SelectRace(appearance);
            return appearance;
        }

        RE::TESRace* LookupFallbackRace()
        {
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            return dataHandler ? dataHandler->LookupForm<RE::TESRace>(kFallbackRaceLocalFormID, kFallbackRacePlugin)
                               : nullptr;
        }

        struct ResolvedRace
        {
            RE::TESRace* race{ nullptr };
            bool own{ false };
        };

        ResolvedRace ResolveRace(Multiplayer::PeerSlot a_playerID, const Multiplayer::AppearanceFormReference& a_race,
            const std::optional<Multiplayer::AppearanceFormReference>& a_defaultRace, std::uint64_t a_revision)
        {
            auto* race = ResolveForm<RE::TESRace>(a_race);
            if (IsPlayableRace(race))
            {
                return ResolvedRace{ .race = race, .own = true };
            }

            auto* fallback = a_defaultRace ? ResolveForm<RE::TESRace>(*a_defaultRace) : nullptr;
            const bool useDefault = IsPlayableRace(fallback);
            logs::warn(
                "Appearance sync race unavailable: player={} revision={} race={}|{:06X} resolved={} "
                "defaultRace={} using={}",
                a_playerID, static_cast<unsigned long long>(a_revision), a_race.plugin, a_race.localFormID,
                race != nullptr,
                a_defaultRace ? std::format("{}|{:06X}", a_defaultRace->plugin, a_defaultRace->localFormID)
                              : std::string{ "none" },
                useDefault ? "default" : "NordRace");
            return ResolvedRace{ .race = useDefault ? fallback : LookupFallbackRace() };
        }

        ResolvedRace ResolveRace(Multiplayer::PeerSlot a_playerID, const AppearanceData& a_appearance)
        {
            return ResolveRace(a_playerID, a_appearance.race, a_appearance.defaultRace, a_appearance.revision);
        }

        RE::BGSHeadPart* FindDefaultHeadPart(FaceRelatedData* a_faceData, std::uint32_t a_type)
        {
            if (!a_faceData || !a_faceData->headParts)
            {
                return nullptr;
            }
            for (auto* headPart : *a_faceData->headParts)
            {
                if (headPart && headPart->type.underlying() == a_type)
                {
                    return headPart;
                }
            }
            return nullptr;
        }

        // The engine's head part visitor (ID 23632) allows one part of each of these types.
        bool IsExclusiveHeadPartType(std::uint32_t a_type)
        {
            switch (a_type)
            {
                case std::to_underlying(HeadPartType::kFace):
                case std::to_underlying(HeadPartType::kEyes):
                case std::to_underlying(HeadPartType::kHair):
                case std::to_underlying(HeadPartType::kFacialHair):
                case std::to_underlying(HeadPartType::kEyebrows):
                    return true;
                default:
                    return false;
            }
        }

        // TESNPC::InitItemImpl (ID 24719) lists a plugin NPC's extra parts after
        // their parent through the visitor at ID 24856, and the head build (ID 25366)
        // attaches only the listed parts. A base built at runtime skips InitItemImpl,
        // so it lists the extras itself, following the same visitor: an exclusive type
        // already present skips the part and its extras. The visitor tests
        // `1 << type` with a 32-bit shift, which wraps mod-added types onto vanilla
        // bits; exclusivity here is tracked by the exact type instead.
        class HeadPartList
        {
        public:
            void Add(RE::BGSHeadPart* a_headPart)
            {
                if (!a_headPart || parts.size() >= kMaximumParts)
                {
                    return;
                }
                const auto type = a_headPart->type.underlying();
                if (IsExclusiveHeadPartType(type))
                {
                    const auto bit = 1U << type;
                    if (exclusiveTypes & bit)
                    {
                        return;
                    }
                    exclusiveTypes |= bit;
                }
                if (std::ranges::contains(parts, a_headPart))
                {
                    return;
                }
                parts.push_back(a_headPart);
                for (auto* extraPart : a_headPart->extraParts)
                {
                    Add(extraPart);
                }
            }

            [[nodiscard]] bool Has(std::uint32_t a_type) const
            {
                return std::ranges::any_of(
                    parts, [a_type](const auto* a_part) { return a_part->type.underlying() == a_type; });
            }

            [[nodiscard]] std::vector<RE::BGSHeadPart*> Take() { return std::move(parts); }

        private:
            // TESNPC::numHeadParts is an int8_t.
            static constexpr std::size_t kMaximumParts = (std::numeric_limits<std::int8_t>::max)();

            std::vector<RE::BGSHeadPart*> parts;
            std::uint32_t exclusiveTypes{ 0 };
        };

        std::vector<RE::BGSHeadPart*> ResolveHeadParts(Multiplayer::PeerSlot a_playerID,
            const AppearanceData& a_appearance, RE::TESRace& a_race, FaceRelatedData* a_faceData)
        {
            HeadPartList headParts;

            for (const auto& reference : a_appearance.headParts)
            {
                auto* headPart = ResolveForm<RE::BGSHeadPart>(reference.form);
                const bool usable = headPart && headPart->type.underlying() == reference.type
                    && (!headPart->validRaces || headPart->validRaces->HasForm(&a_race));
                if (usable)
                {
                    headParts.Add(headPart);
                    continue;
                }

                auto* fallback = FindDefaultHeadPart(a_faceData, reference.type);
                logs::warn(
                    "Appearance sync replaced unavailable head part: player={} revision={} part={}|{:06X} type={} "
                    "fallback={:08X}",
                    a_playerID, static_cast<unsigned long long>(a_appearance.revision), reference.form.plugin,
                    reference.form.localFormID, reference.type, fallback ? fallback->GetFormID() : 0);
                headParts.Add(fallback);
            }

            const auto faceType = std::to_underlying(HeadPartType::kFace);
            if (!headParts.Has(faceType))
            {
                headParts.Add(FindDefaultHeadPart(a_faceData, faceType));
            }
            return headParts.Take();
        }

        RE::BGSColorForm* CreateHairColor(const AppearanceColor& a_color)
        {
            auto* colorForm = DynamicForm::Create<RE::BGSColorForm>();
            if (!colorForm)
            {
                return nullptr;
            }
            colorForm->color = RE::Color{ a_color.red, a_color.green, a_color.blue, 0 };
            return colorForm;
        }

        std::uint16_t FindTintPreset(const TintAsset& a_asset, const AppearanceColor& a_color)
        {
            const auto& colors = a_asset.presets.colors;
            for (std::uint32_t i = 0; i < colors.size(); ++i)
            {
                if (colors[i] && ToAppearanceColor(colors[i]->color) == a_color)
                {
                    return static_cast<std::uint16_t>(i);
                }
            }
            return 0;
        }

        // BGSDefaultObjectManager::GetObject is unusable here because windows.h
        // defines GetObject as a macro.
        RE::TESCombatStyle* GetDefaultCombatStyle()
        {
            constexpr std::size_t kCombatStyleIndex = RE::DEFAULT_OBJECTS::kCombatStyle;
            const auto* defaults = RE::BGSDefaultObjectManager::GetSingleton();
            auto* form = defaults && defaults->IsObjectInitialized(kCombatStyleIndex)
                ? defaults->objects[kCombatStyleIndex]
                : nullptr;
            return form ? form->As<RE::TESCombatStyle>() : nullptr;
        }

        // The TESNPC constructor already runs InitializeData, which leaves no class,
        // AI packages, factions, inventory, or outfits, and points faceData at a
        // shared engine default that must be replaced rather than written.
        // a_tintTextures receives the texture each of the base's tint layers renders.
        RE::TESNPC* BuildActorBase(Multiplayer::PeerSlot a_playerID, const AppearanceData& a_appearance,
            RE::TESRace& a_race, std::vector<RE::BSFixedString>& a_tintTextures)
        {
            auto* npc = DynamicForm::Create<RE::TESNPC>();
            if (!npc)
            {
                logs::error("Appearance sync could not create an actor base: player={} revision={}", a_playerID,
                    static_cast<unsigned long long>(a_appearance.revision));
                return nullptr;
            }

            auto* faceData = GetFaceRelatedData(&a_race, a_appearance.female);
            npc->headRelatedData = new RE::TESNPC::HeadRelatedData();
            npc->faceData = RE::calloc<RE::TESNPC::FaceData>(1);
            npc->tintLayers = new RE::BSTArray<RE::TESNPC::Layer*>();

            npc->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kUnique);
            npc->fullName = PlayerLabel::Format(a_appearance.name);
            npc->race = &a_race;
            npc->originalRace = &a_race;
            // TESNPC::InitItemImpl completes these only for NPCs loaded from plugins:
            // an NPC without attack data takes its race's (BGSAttackDataForm::InitItem),
            // and one without a combat style gets the default object. Weapon draws
            // read the attack data map without a null check.
            npc->attackDataMap = a_race.attackDataMap;
            npc->combatStyle = GetDefaultCombatStyle();
            npc->actorData.actorBaseFlags.set(a_appearance.female, RE::ACTOR_BASE_DATA::Flag::kFemale);
            npc->weight = a_appearance.weight;
            npc->height = std::clamp(a_appearance.height, kMinimumRemoteHeight, kMaximumRemoteHeight);

            const auto headParts = ResolveHeadParts(a_playerID, a_appearance, a_race, faceData);
            if (!headParts.empty())
            {
                npc->headParts = RE::calloc<RE::BGSHeadPart*>(headParts.size());
                std::ranges::copy(headParts, npc->headParts);
                npc->numHeadParts = static_cast<std::int8_t>(headParts.size());
            }

            RE::BGSColorForm* hairColor = nullptr;
            if (a_appearance.hairColor)
            {
                hairColor = CreateHairColor(*a_appearance.hairColor);
            }
            if (!hairColor && faceData)
            {
                hairColor = faceData->defaultHairColor;
            }
            npc->headRelatedData->hairColor = hairColor;

            auto* faceDetails = ResolveForm<RE::BGSTextureSet>(a_appearance.faceDetails);
            if (!faceDetails && faceData)
            {
                faceDetails = faceData->defaultFaceDetailsTextureSet;
            }
            npc->headRelatedData->faceDetails = faceDetails;

            std::ranges::copy(a_appearance.faceMorphs, npc->faceData->morphs);
            std::ranges::copy(a_appearance.faceParts, npc->faceData->parts);
            npc->bodyTintColor.red = a_appearance.bodyTint.red;
            npc->bodyTintColor.green = a_appearance.bodyTint.green;
            npc->bodyTintColor.blue = a_appearance.bodyTint.blue;

            std::size_t droppedTints = 0;
            std::size_t missingTextures = 0;
            const AppearanceTint* firstMissing = nullptr;
            a_tintTextures.clear();
            for (const auto& tint : a_appearance.tints)
            {
                const auto* asset = FindTintAsset(faceData, tint.index, tint.type);
                if (!asset)
                {
                    ++droppedTints;
                    continue;
                }
                const auto& assetTexture = asset->texture.file.textureName;
                const bool assetPath = tint.texture.empty() || AsciiCase::Equals(tint.texture, assetTexture.c_str());
                if (!assetPath && !Forms::TextureExists(tint.texture))
                {
                    firstMissing = firstMissing ? firstMissing : std::addressof(tint);
                    ++missingTextures;
                    continue;
                }
                a_tintTextures.emplace_back(assetPath ? assetTexture : RE::BSFixedString{ tint.texture });
                auto* layer = RE::calloc<RE::TESNPC::Layer>(1);
                layer->tintColor = RE::Color{ tint.color.red, tint.color.green, tint.color.blue, 0 };
                layer->tintIndex = tint.index;
                layer->preset = FindTintPreset(*asset, tint.color);
                layer->interpolationValue
                    = static_cast<std::uint16_t>(std::lround(std::clamp(tint.alpha, 0.0F, 1.0F) * 100.0F));
                npc->tintLayers->push_back(layer);
            }

            const auto extraParts
                = std::ranges::count_if(headParts, [](auto* a_part) { return a_part->IsExtraPart(); });
            logs::debug(
                "Appearance sync built actor base: player={} revision={} base={:08X} name=\"{}\" race={:08X} female={} "
                "weight={:.1f} height={:.2f} headParts={}/{} extraParts={} hairColor={:08X} faceDetails={:08X} "
                "tints={}/{} droppedTints={} missingTintTextures={} firstMissing=\"{}\" attackData={} "
                "combatStyle={:08X}",
                a_playerID, static_cast<unsigned long long>(a_appearance.revision), npc->GetFormID(), a_appearance.name,
                a_race.GetFormID(), a_appearance.female, a_appearance.weight, npc->height,
                headParts.size() - static_cast<std::size_t>(extraParts), a_appearance.headParts.size(), extraParts,
                hairColor ? hairColor->GetFormID() : 0, faceDetails ? faceDetails->GetFormID() : 0,
                npc->tintLayers->size(), a_appearance.tints.size(), droppedTints, missingTextures,
                firstMissing ? std::string_view{ firstMissing->texture } : std::string_view{}, npc->attackDataMap != nullptr,
                npc->combatStyle ? npc->combatStyle->GetFormID() : 0);
            return npc;
        }

        AppearanceData MakeDefaultAppearance(RE::TESRace& a_race, bool a_female)
        {
            AppearanceData appearance{ .race = MakeFormReference(&a_race).value_or(AppearanceFormReference{}),
                .female = a_female };
            const auto* faceData = GetFaceRelatedData(&a_race, a_female);
            if (!faceData)
            {
                return appearance;
            }

            if (faceData->headParts)
            {
                for (const auto* headPart : *faceData->headParts)
                {
                    if (const auto reference = MakeFormReference(headPart))
                    {
                        appearance.headParts.push_back(
                            AppearanceHeadPart{ .form = *reference, .type = headPart->type.underlying() });
                    }
                }
            }

            if (faceData->tintMasks)
            {
                using SkinTone = TintAsset::TintLayer::SkinTone;
                for (const auto* asset : *faceData->tintMasks)
                {
                    const auto* color = asset ? asset->texture.presetDefault : nullptr;
                    if (color && asset->texture.skinTone == SkinTone::kSkinTone)
                    {
                        appearance.bodyTint = ToAppearanceColor(color->color);
                        appearance.tints.push_back(AppearanceTint{ .index = asset->texture.index,
                            .type = static_cast<std::uint8_t>(SkinTone::kSkinTone),
                            .color = appearance.bodyTint,
                            .alpha = 1.0F });
                        break;
                    }
                }
            }
            return appearance;
        }

        RE::TESNPC* CreateActorBase(Multiplayer::PeerSlot a_playerID, const AppearanceData& a_appearance,
            std::vector<RE::BSFixedString>& a_tintTextures)
        {
            const auto resolved = ResolveRace(a_playerID, a_appearance);
            if (!resolved.race)
            {
                logs::error("Appearance sync could not resolve any race: player={} revision={}", a_playerID,
                    static_cast<unsigned long long>(a_appearance.revision));
                return nullptr;
            }
            if (resolved.own)
            {
                return BuildActorBase(a_playerID, a_appearance, *resolved.race, a_tintTextures);
            }

            auto look = MakeDefaultAppearance(*resolved.race, a_appearance.female);
            look.name = a_appearance.name;
            look.level = a_appearance.level;
            look.weight = a_appearance.weight;
            look.height = a_appearance.height;
            look.revision = a_appearance.revision;
            return BuildActorBase(a_playerID, look, *resolved.race, a_tintTextures);
        }

        RE::TESNPC* CreateDefaultActorBase(
            Multiplayer::PeerSlot a_playerID, std::vector<RE::BSFixedString>& a_tintTextures)
        {
            auto* race = LookupFallbackRace();
            if (!race)
            {
                logs::error("Appearance sync could not resolve NordRace: player={}", a_playerID);
                return nullptr;
            }
            return BuildActorBase(a_playerID, MakeDefaultAppearance(*race, false), *race, a_tintTextures);
        }

        // The beast races have no head data, so a beast base takes only the player's
        // label, sex, weight, and height.
        RE::TESNPC* CreateBeastBase(
            Multiplayer::PeerSlot a_playerID, const std::optional<AppearanceData>& a_appearance, RE::TESRace& a_race)
        {
            AppearanceData look{ .race = MakeFormReference(&a_race).value_or(AppearanceFormReference{}) };
            if (a_appearance)
            {
                look.revision = a_appearance->revision;
                look.name = a_appearance->name;
                look.level = a_appearance->level;
                look.female = a_appearance->female;
                look.weight = a_appearance->weight;
                look.height = a_appearance->height;
            }
            std::vector<RE::BSFixedString> tintTextures;
            return BuildActorBase(a_playerID, look, a_race, tintTextures);
        }

        RE::NiSourceTexture* FindFaceTintTexture(RE::Actor& a_actor, const RE::TESNPC& a_base)
        {
            auto* faceNode = a_actor.GetFaceNodeSkinned();
            if (!faceNode || !a_base.headParts)
            {
                return nullptr;
            }

            const RE::BGSHeadPart* face = nullptr;
            for (std::int8_t i = 0; i < a_base.numHeadParts && !face; ++i)
            {
                const auto* headPart = a_base.headParts[i];
                if (headPart && headPart->type == HeadPartType::kFace)
                {
                    face = headPart;
                }
            }

            // PrepareHeadPartForShaders (ID 26838, SE 26259) names the face geometry
            // after the face head part's editor ID.
            auto* object = face ? faceNode->GetObjectByName(face->formEditorID) : nullptr;
            auto* geometry = object ? object->AsGeometry() : nullptr;
            auto* shader = geometry ? netimmerse_cast<RE::BSLightingShaderProperty*>(
                                          geometry->GetGeometryRuntimeData().shaderProperty.get())
                                    : nullptr;
            auto* material = shader ? shader->material : nullptr;
            if (!material || material->GetFeature() != RE::BSShaderMaterial::Feature::kFaceGen)
            {
                return nullptr;
            }
            return static_cast<RE::BSLightingShaderMaterialFacegen*>(material)->tintTexture.get();
        }

        // The render (ID 27040, SE 26454) reads only a mask texture's textureName and
        // loads the file by it (ID 76005, SE 74273) before it returns, so a path that
        // no race asset holds can be passed in a stand-in with TESTexture's layout.
        struct MaskTexture
        {
            const void* vtable;
            RE::BSFixedString textureName;
        };
        static_assert(sizeof(MaskTexture) == sizeof(RE::TESTexture));

        // PrepareHeadPartForShaders (ID 26838, SE 26259) gives a runtime-built face a
        // blank tint texture and renders tint masks into it only for the player base,
        // or for other NPCs when bUseFaceGenPreprocessedHeads is off. Remote actor
        // bases get neither, so the blank texture darkens the face. Queue the same
        // render the engine uses (ID 27040, SE 26454) from the remote actor base's
        // tint layers. The SE IDs were matched against the 1.5.97 disassembly by their
        // code and callers.
        std::size_t QueueFaceTintRender(const RE::TESNPC& a_base, std::span<const RE::BSFixedString> a_layerTextures,
            RE::NiSourceTexture& a_texture)
        {
            PROFILE_FUNCTION();
            // A render job has 16 layer slots and does not check the mask count.
            constexpr std::size_t kMaximumRenderedTints = 16;
            using RenderTintMasks = bool(RE::BSTArray<RE::TintMask*>*, RE::NiTexture*);
            static REL::Relocation<RenderTintMasks> renderTintMasks{ RELOCATION_ID(26454, 27040) };

            auto* race = a_base.race;
            const auto* faceData = GetFaceRelatedData(race, a_base.IsFemale());
            std::array<RE::TintMask, kMaximumRenderedTints> masks{};
            std::array<MaskTexture, kMaximumRenderedTints> textures{};
            RE::BSTArray<RE::TintMask*> maskList;
            if (a_base.tintLayers)
            {
                for (std::uint32_t i = 0; i < a_base.tintLayers->size() && i < a_layerTextures.size(); ++i)
                {
                    if (maskList.size() >= kMaximumRenderedTints)
                    {
                        break;
                    }
                    const auto* layer = (*a_base.tintLayers)[i];
                    const auto* asset = layer ? FindTintAsset(faceData, layer->tintIndex) : nullptr;
                    if (!asset || layer->interpolationValue == 0)
                    {
                        continue;
                    }
                    auto& texture = textures[maskList.size()];
                    texture.vtable = *reinterpret_cast<const void* const*>(std::addressof(asset->texture.file));
                    texture.textureName = a_layerTextures[i];
                    auto& mask = masks[maskList.size()];
                    mask.texture = reinterpret_cast<RE::TESTexture*>(std::addressof(texture));
                    mask.color = layer->tintColor;
                    mask.alpha = static_cast<float>(layer->interpolationValue) / 100.0F;
                    mask.type = static_cast<RE::TintMask::Type>(asset->texture.skinTone.underlying());
                    maskList.push_back(std::addressof(mask));
                }
            }

            // The job copies the textures and colors, so the masks may go out of scope.
            renderTintMasks(std::addressof(maskList), std::addressof(a_texture));
            return maskList.size();
        }
    }

    void SyncAppearance(const LocalFrame& a_local, bool a_transformed, float a_delta)
    {
        PROFILE_FUNCTION();
        static float captureElapsed = kCaptureIntervalSeconds;
        static float publishElapsed = kMinimumPublishIntervalSeconds;
        static std::optional<AppearanceData> published;
        static std::optional<AppearanceData> candidate;
        static std::uint64_t revision = 0;
        static std::uint64_t publishedConnectionGeneration = 0;
        static float unsettledSeconds = 0.0F;
        static bool unsettledReported = false;
        static std::string invalidReported;

        const float delta = FrameTime::Clamp(a_delta);
        captureElapsed += delta;
        publishElapsed += delta;

        if (!Network::IsConnected())
        {
            return;
        }
        if (const auto generation = Network::GetConnectionGeneration(); generation != publishedConnectionGeneration)
        {
            publishedConnectionGeneration = generation;
            if (published)
            {
                logs::debug("Appearance sync republishing after connect: revision={} connectionGeneration={}",
                    static_cast<unsigned long long>(published->revision), static_cast<unsigned long long>(generation));
                Network::PublishLocalAppearance(*published);
                publishElapsed = 0.0F;
            }
        }

        if (a_transformed && published)
        {
            // A beast race's originalRace is the player record's template race, not the
            // player's own, and a revision would respawn the beast on every peer.
            candidate.reset();
            return;
        }
        if (captureElapsed < kCaptureIntervalSeconds)
        {
            return;
        }
        const float captureInterval = captureElapsed;
        captureElapsed = 0.0F;

        auto captured = CaptureLocalAppearance(a_local.player);
        if (!captured)
        {
            return;
        }
        if (published && published->HasSameLook(*captured) && published->HasSameLabel(*captured))
        {
            candidate.reset();
            unsettledSeconds = 0.0F;
            unsettledReported = false;
            return;
        }
        if (!candidate || !candidate->HasSameLook(*captured) || !candidate->HasSameLabel(*captured))
        {
            if (candidate)
            {
                unsettledSeconds += captureInterval;
                if (!unsettledReported && unsettledSeconds >= kUnsettledCaptureWarningSeconds)
                {
                    unsettledReported = true;
                    logs::warn(
                        "Appearance sync capture keeps changing, not publishing: unsettled={:.0f}s published={} "
                        "changes={}",
                        unsettledSeconds, published.has_value(), DescribeCaptureChange(*candidate, *captured));
                }
            }
            candidate = std::move(captured);
            return;
        }
        if (publishElapsed < kMinimumPublishIntervalSeconds)
        {
            return;
        }

        if (revision >= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
        {
            logs::critical("Appearance sync cannot advance revision beyond the protocol limit");
            return;
        }
        candidate->revision = ++revision;
        if (!Multiplayer::IsValidAppearance(*candidate))
        {
            if (auto reason = DescribeInvalidAppearance(*candidate); reason != invalidReported)
            {
                logs::warn("Appearance sync skipped invalid local appearance: revision={} invalid={}",
                    static_cast<unsigned long long>(revision), reason);
                invalidReported = std::move(reason);
            }
            candidate.reset();
            return;
        }

        logs::debug(
            "Appearance sync publishing local appearance: revision={} level={} race={}|{:06X} female={} headParts={} "
            "tints={} overlays={}",
            static_cast<unsigned long long>(candidate->revision), candidate->level, candidate->race.plugin,
            candidate->race.localFormID, candidate->female, candidate->headParts.size(), candidate->tints.size(),
            candidate->overlays.size());
        Network::PublishLocalAppearance(*candidate);
        published = std::move(candidate);
        candidate.reset();
        publishElapsed = 0.0F;
        unsettledSeconds = 0.0F;
        unsettledReported = false;
        invalidReported.clear();
    }

    std::string GetRaceName(Multiplayer::PeerSlot a_playerID, const Multiplayer::AppearanceFormReference& a_race,
        const std::optional<Multiplayer::AppearanceFormReference>& a_defaultRace)
    {
        const auto* race = ResolveRace(a_playerID, a_race, a_defaultRace, 0).race;
        const char* name = race ? race->GetFullName() : nullptr;
        return name ? name : "";
    }

    void AcceptRemoteAppearance(
        RemoteState& a_remote, Multiplayer::PeerSlot a_playerID, Multiplayer::PlayerAppearance&& a_appearance)
    {
        if (a_remote.latest && a_appearance.revision <= a_remote.latest->revision)
        {
            return;
        }

        const bool relabelOnly = a_remote.latest && a_remote.latest->HasSameLook(a_appearance);
        logs::debug(
            "Appearance sync accepted remote appearance: player={} revision={} name=\"{}\" level={} race={}|{:06X} "
            "female={} headParts={} tints={} overlays={} relabelOnly={}",
            a_playerID, static_cast<unsigned long long>(a_appearance.revision), a_appearance.name, a_appearance.level,
            a_appearance.race.plugin, a_appearance.race.localFormID, a_appearance.female, a_appearance.headParts.size(),
            a_appearance.tints.size(), a_appearance.overlays.size(), relabelOnly);

        if (relabelOnly)
        {
            const auto previousRevision = a_remote.latest->revision;
            for (auto* base : { a_remote.base, a_remote.beastBase })
            {
                if (base)
                {
                    base->fullName = PlayerLabel::Format(a_appearance.name);
                }
            }
            if (a_remote.baseRevision == previousRevision)
            {
                a_remote.baseRevision = a_appearance.revision;
            }
            if (a_remote.beastBaseRevision == previousRevision)
            {
                a_remote.beastBaseRevision = a_appearance.revision;
            }
            if (a_remote.spawnedRevision == previousRevision)
            {
                a_remote.spawnedRevision = a_appearance.revision;
            }
        }
        a_remote.latest = std::move(a_appearance);
    }

    void SyncAppearance(RemoteState& a_remote, float a_delta)
    {
        const float delta = FrameTime::Clamp(a_delta);
        if (!a_remote.latest)
        {
            a_remote.appearanceWaitElapsed += delta;
        }
        a_remote.respawnCooldown = (std::max)(a_remote.respawnCooldown - delta, 0.0F);
        a_remote.shapeshiftCooldown = (std::max)(a_remote.shapeshiftCooldown - delta, 0.0F);
    }

    SpawnBase GetSpawnBase(RemoteState& a_remote, Multiplayer::PeerSlot a_playerID, Multiplayer::Shapeshift a_form)
    {
        if (!a_remote.latest && a_remote.appearanceWaitElapsed < kFirstAppearanceWaitSeconds)
        {
            return {};
        }

        const auto revision = a_remote.latest ? a_remote.latest->revision : 0;
        if (a_form != Multiplayer::Shapeshift::kNone && a_form != a_remote.failedForm)
        {
            if (!a_remote.beastBase || a_remote.beastBaseForm != a_form || a_remote.beastBaseRevision != revision)
            {
                auto* race = Forms::GetBeastRace(a_form);
                auto* base = race ? CreateBeastBase(a_playerID, a_remote.latest, *race) : nullptr;
                if (!base)
                {
                    logs::error("Appearance sync could not build a beast base; showing the player's own look: "
                                "player={} form={} raceResolved={}",
                        a_playerID, Multiplayer::GetShapeshiftName(a_form), race != nullptr);
                    a_remote.failedForm = a_form;
                }
                else
                {
                    a_remote.beastBase = base;
                    a_remote.beastBaseForm = a_form;
                    a_remote.beastBaseRevision = revision;
                }
            }
            if (a_remote.beastBase && a_remote.beastBaseForm == a_form)
            {
                return { .base = a_remote.beastBase, .form = a_form };
            }
        }

        if ((!a_remote.base || a_remote.baseRevision != revision) && a_remote.failedRevision != revision)
        {
            std::vector<RE::BSFixedString> tintTextures;
            auto* base = a_remote.latest ? CreateActorBase(a_playerID, *a_remote.latest, tintTextures)
                                         : CreateDefaultActorBase(a_playerID, tintTextures);
            if (base)
            {
                a_remote.base = base;
                a_remote.baseRevision = revision;
                a_remote.baseTintTextures = std::move(tintTextures);
            }
            else
            {
                a_remote.failedRevision = revision;
            }
        }

        return { .base = a_remote.base };
    }

    void OnActorSpawned(RemoteState& a_remote, const SpawnBase& a_spawned)
    {
        // A failed build also counts as spawned, so the actor is not retired every frame for
        // an appearance that cannot be built.
        a_remote.spawnedRevision = a_remote.latest ? a_remote.latest->revision : 0;
        a_remote.spawnedBase = a_spawned.base;
        a_remote.spawnedForm = a_spawned.form;
        a_remote.respawnCooldown = kMinimumRespawnIntervalSeconds;
        a_remote.shapeshiftCooldown = kMinimumShapeshiftIntervalSeconds;
        a_remote.colorsApplied = false;
        a_remote.faceTintTexture.reset();
    }

    bool RequiresRespawn(const RemoteState& a_remote)
    {
        return a_remote.latest && a_remote.latest->revision != a_remote.spawnedRevision
            && a_remote.respawnCooldown <= 0.0F;
    }

    bool RequiresShapeshift(const RemoteState& a_remote, Multiplayer::Shapeshift a_form)
    {
        const auto shownForm = a_form == a_remote.failedForm ? Multiplayer::Shapeshift::kNone : a_form;
        return shownForm != a_remote.spawnedForm && a_remote.shapeshiftCooldown <= 0.0F;
    }

    void ApplyActorPresentation(
        RemoteState& a_remote, Multiplayer::PeerSlot a_playerID, RE::Actor& a_actor, bool a_actorReady)
    {
        PROFILE_FUNCTION();
        if (!a_actorReady || !a_actor.Is3DLoaded() || !a_remote.spawnedBase || a_remote.spawnedBase != a_remote.base
            || a_actor.GetActorBase() != a_remote.spawnedBase)
        {
            return;
        }

        if (!a_remote.colorsApplied)
        {
            PROFILE_SCOPE("ApplyActorPresentation colors");
            a_remote.colorsApplied = true;
            a_actor.UpdateHairColor();
            a_actor.UpdateSkinColor();
            // A newer revision waiting for its respawn has a different look.
            if (a_remote.latest && a_remote.latest->revision == a_remote.spawnedRevision)
            {
                RaceMenuOverlays::Apply(a_actor, a_remote.latest->overlays);
            }
            logs::debug(
                "Appearance sync applied actor colors: player={} actor={:08X} base={:08X} revision={} faceNode={}",
                a_playerID, a_actor.GetHandle().native_handle(), a_remote.spawnedBase->GetFormID(),
                static_cast<unsigned long long>(a_remote.spawnedRevision), a_actor.GetFaceNodeSkinned() != nullptr);
        }

        // A head rebuilt by a 3D reset gets a new blank texture, so render again.
        auto* tintTexture = FindFaceTintTexture(a_actor, *a_remote.spawnedBase);
        if (!tintTexture || tintTexture == a_remote.faceTintTexture.get())
        {
            return;
        }
        a_remote.faceTintTexture.reset(tintTexture);
        const auto renderedTints = QueueFaceTintRender(*a_remote.spawnedBase, a_remote.baseTintTextures, *tintTexture);
        logs::debug(
            "Appearance sync queued actor face tint: player={} actor={:08X} base={:08X} revision={} tints={}/{}",
            a_playerID, a_actor.GetHandle().native_handle(), a_remote.spawnedBase->GetFormID(),
            static_cast<unsigned long long>(a_remote.spawnedRevision), renderedTints,
            a_remote.spawnedBase->tintLayers ? a_remote.spawnedBase->tintLayers->size() : 0);
    }
}
