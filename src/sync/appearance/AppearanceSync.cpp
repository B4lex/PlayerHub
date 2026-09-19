#include "sync/appearance/AppearanceSync.h"
#include "forms/FormLookup.h"
#include "network/SocketIOClient.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AppearanceSync
{
    namespace
    {
        constexpr float kCaptureInterval = 1.0F;
        constexpr float kMinimumPublishInterval = 3.0F;
        constexpr float kFirstAppearanceWaitSeconds = 3.0F;
        constexpr float kMinimumRespawnInterval = 2.0F;
        // Vanilla fallback when the source race is unavailable locally.
        constexpr std::string_view kFallbackRacePlugin = "Skyrim.esm";
        constexpr RE::FormID kFallbackRaceLocalFormID = 0x00013746; // NordRace

        using Multiplayer::AppearanceColor;
        using Multiplayer::AppearanceFormReference;
        using Multiplayer::AppearanceHeadPart;
        using Multiplayer::AppearanceTint;
        using AppearanceData = Multiplayer::PlayerAppearance;
        using FormLookup::MakeFormReference;
        using FormLookup::ResolveForm;
        using HeadPartType = RE::BGSHeadPart::HeadPartType;
        using FaceRelatedData = RE::TESRace::FaceRelatedData;
        using TintAsset = FaceRelatedData::TintAsset;

        constexpr AppearanceColor ToAppearanceColor(const RE::Color& a_color) noexcept
        {
            return AppearanceColor{ .red = a_color.red, .green = a_color.green, .blue = a_color.blue };
        }

        bool EqualsIgnoreCase(std::string_view a_first, std::string_view a_second)
        {
            return std::ranges::equal(a_first, a_second, [](char a_left, char a_right) {
                return std::tolower(static_cast<unsigned char>(a_left))
                    == std::tolower(static_cast<unsigned char>(a_right));
            });
        }

        bool IsPlayableRace(const RE::TESRace* a_race)
        {
            return a_race && a_race->data.flags.all(RE::RACE_DATA::Flag::kPlayable);
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
                if (asset && EqualsIgnoreCase(asset->texture.file.textureName.c_str(), maskPath))
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

        // Returns the player's tint masks in blend order. CommonLibSSE-NG reads
        // PlayerCharacter::tintMasks at 0xB18 on AE, but 1.7.104 keeps it at 0xB20
        // (PlayerCharacter::GetNumTints, ID 40700). The array is used only when it
        // agrees with the engine's own lookups; otherwise masks are listed by type.
        std::vector<RE::TintMask*> GetPlayerTintMasks(RE::PlayerCharacter& a_player)
        {
            if (!REL::Module::IsAE())
            {
                const auto& tintMasks = a_player.GetPlayerRuntimeData().tintMasks;
                return std::vector<RE::TintMask*>(tintMasks.begin(), tintMasks.end());
            }

            using GetNumTints = std::uint32_t(RE::PlayerCharacter*, std::uint32_t);
            using GetTintMask = RE::TintMask*(RE::PlayerCharacter*, std::uint32_t, std::uint32_t);
            static REL::Relocation<GetNumTints> getNumTints{ REL::ID(40700) };
            static REL::Relocation<GetTintMask> getTintMask{ REL::ID(40698) };
            constexpr auto kTypeCount = static_cast<std::uint32_t>(RE::TintMask::Type::kTotal);

            std::array<std::uint32_t, kTypeCount> countByType{};
            std::uint32_t total = 0;
            for (std::uint32_t type = 0; type < kTypeCount; ++type)
            {
                countByType[type] = getNumTints(std::addressof(a_player), type);
                total += countByType[type];
            }

            std::vector<RE::TintMask*> masks;
            const auto& tintMasks = REL::RelocateMember<RE::BSTArray<RE::TintMask*>>(std::addressof(a_player), 0xB20);
            bool consistent = tintMasks.size() == total;
            std::array<std::uint32_t, kTypeCount> seenByType{};
            for (std::uint32_t i = 0; consistent && i < tintMasks.size(); ++i)
            {
                auto* mask = tintMasks[i];
                const auto type = mask ? mask->type.underlying() : kTypeCount;
                consistent
                    = type < kTypeCount && getTintMask(std::addressof(a_player), type, seenByType[type]++) == mask;
                masks.push_back(mask);
            }
            if (consistent)
            {
                return masks;
            }

            static bool loggedFallback = false;
            if (!loggedFallback)
            {
                loggedFallback = true;
                logs::warn(
                    "Appearance sync could not read the player tint mask array; listing {} masks by type", total);
            }
            masks.clear();
            for (std::uint32_t type = 0; type < kTypeCount; ++type)
            {
                for (std::uint32_t i = 0; i < countByType[type]; ++i)
                {
                    masks.push_back(getTintMask(std::addressof(a_player), type, i));
                }
            }
            return masks;
        }

        // Removes control characters and cuts an overlong name at a UTF-8 code point
        // boundary, so an unusual name set by a mod still synchronizes.
        std::string SanitizeName(std::string_view a_name)
        {
            std::string name;
            for (const auto character : a_name)
            {
                const auto byte = static_cast<unsigned char>(character);
                if (byte >= 0x20 && byte != 0x7F)
                {
                    name.push_back(character);
                }
            }
            if (name.size() > AppearanceData::kMaximumNameLength)
            {
                auto length = AppearanceData::kMaximumNameLength;
                // Back up over continuation bytes (10xxxxxx) to the start of the cut
                // code point.
                while (length > 0 && (static_cast<unsigned char>(name[length]) & 0xC0) == 0x80)
                {
                    --length;
                }
                name.resize(length);
            }
            return name;
        }

        // Logs a capture failure only when its reason changes, so a persistent
        // failure does not flood the log once per second.
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

        std::optional<AppearanceData> CaptureLocalAppearance(RE::PlayerCharacter& a_player)
        {
            auto* base = a_player.GetActorBase();
            if (!base)
            {
                LogCaptureFailure("player has no actor base");
                return std::nullopt;
            }

            // Transformation races replace the skeleton and behavior graph. Keep the
            // last playable appearance instead of publishing a beast form.
            auto* race = base->race;
            if (!IsPlayableRace(race))
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

            const auto* fullName = base->GetFullName();
            AppearanceData appearance{ .name = SanitizeName(fullName ? fullName : ""),
                .race = *raceReference,
                .female = base->IsFemale(),
                .weight = std::clamp(base->weight, 0.0F, AppearanceData::kMaximumWeight),
                .height = std::clamp(base->height, AppearanceData::kMinimumHeight, AppearanceData::kMaximumHeight) };

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
                    if (!reference || appearance.headParts.size() >= AppearanceData::kMaximumHeadParts)
                    {
                        continue;
                    }
                    appearance.headParts.push_back(AppearanceHeadPart{
                        .form = *reference, .type = static_cast<std::uint8_t>(headPart->type.underlying()) });
                }
            }

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
                    appearance.faceParts[i] = base->faceData->parts[i];
                }
            }

            appearance.bodyTint = ToAppearanceColor(base->bodyTintColor);
            // The player's face tints live on PlayerCharacter rather than TESNPC. The
            // runtime-data layout is only known for flat Skyrim.
            if (!REL::Module::IsVR())
            {
                const auto* faceData = GetFaceRelatedData(race, appearance.female);
                for (const auto* mask : GetPlayerTintMasks(a_player))
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
                    appearance.tints.push_back(AppearanceTint{ .index = asset->texture.index,
                        .type = type,
                        .color = ToAppearanceColor(mask->color),
                        .alpha = (std::min)(mask->alpha, 1.0F) });
                }
            }

            return appearance;
        }

        bool AssignDynamicFormID(RE::TESForm& a_form)
        {
            if (a_form.GetFormID() != 0)
            {
                return true;
            }

            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            const auto formID = dataHandler ? dataHandler->GetNextID() : 0;
            if (formID == 0)
            {
                return false;
            }
            a_form.SetFormID(formID, false);
            return a_form.GetFormID() == formID;
        }

        RE::TESRace* LookupFallbackRace()
        {
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            return dataHandler ? dataHandler->LookupForm<RE::TESRace>(kFallbackRaceLocalFormID, kFallbackRacePlugin)
                               : nullptr;
        }

        RE::TESRace* ResolveRace(const std::string& a_playerID, const AppearanceData& a_appearance)
        {
            auto* race = ResolveForm<RE::TESRace>(a_appearance.race);
            if (IsPlayableRace(race))
            {
                return race;
            }

            logs::warn("Appearance sync falling back to NordRace: player={} revision={} race={}|{:06X} resolved={}",
                a_playerID, static_cast<unsigned long long>(a_appearance.revision), a_appearance.race.plugin,
                a_appearance.race.localFormID, race != nullptr);
            return LookupFallbackRace();
        }

        RE::BGSHeadPart* FindDefaultHeadPart(FaceRelatedData* a_faceData, std::uint8_t a_type)
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

        std::vector<RE::BGSHeadPart*> ResolveHeadParts(const std::string& a_playerID,
            const AppearanceData& a_appearance, RE::TESRace& a_race, FaceRelatedData* a_faceData)
        {
            std::vector<RE::BGSHeadPart*> headParts;
            const auto addUnique = [&headParts](RE::BGSHeadPart* a_headPart) {
                if (a_headPart && std::ranges::find(headParts, a_headPart) == headParts.end())
                {
                    headParts.push_back(a_headPart);
                }
            };

            for (const auto& reference : a_appearance.headParts)
            {
                auto* headPart = ResolveForm<RE::BGSHeadPart>(reference.form);
                const bool usable = headPart && headPart->type.underlying() == reference.type
                    && (!headPart->validRaces || headPart->validRaces->HasForm(&a_race));
                if (usable)
                {
                    addUnique(headPart);
                    continue;
                }

                auto* fallback = FindDefaultHeadPart(a_faceData, reference.type);
                logs::warn(
                    "Appearance sync replaced unavailable head part: player={} revision={} part={}|{:06X} type={} "
                    "fallback={:08X}",
                    a_playerID, static_cast<unsigned long long>(a_appearance.revision), reference.form.plugin,
                    reference.form.localFormID, reference.type, fallback ? fallback->GetFormID() : 0);
                addUnique(fallback);
            }

            const auto faceType = static_cast<std::uint8_t>(HeadPartType::kFace);
            const bool hasFace = std::ranges::any_of(
                headParts, [faceType](const auto* a_part) { return a_part->type.underlying() == faceType; });
            if (!hasFace)
            {
                addUnique(FindDefaultHeadPart(a_faceData, faceType));
            }
            return headParts;
        }

        RE::BGSColorForm* CreateHairColor(const AppearanceColor& a_color)
        {
            auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::BGSColorForm>();
            auto* colorForm = factory ? factory->Create() : nullptr;
            if (!colorForm || !AssignDynamicFormID(*colorForm))
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

        // Builds a runtime actor base from engine defaults instead of copying a template
        // record, so remote actors do not depend on any particular base form. The
        // TESNPC constructor already runs InitializeData, which leaves no class, AI
        // packages, factions, inventory, or outfits, and points faceData at a shared
        // engine default that must be replaced rather than written.
        RE::TESNPC* CreateActorBase(const std::string& a_playerID, const AppearanceData& a_appearance)
        {
            auto* race = ResolveRace(a_playerID, a_appearance);
            if (!race)
            {
                logs::error("Appearance sync could not resolve any race: player={} revision={}", a_playerID,
                    static_cast<unsigned long long>(a_appearance.revision));
                return nullptr;
            }

            auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESNPC>();
            auto* npc = factory ? factory->Create() : nullptr;
            if (!npc || !AssignDynamicFormID(*npc))
            {
                logs::error("Appearance sync could not create an actor base: player={} revision={}", a_playerID,
                    static_cast<unsigned long long>(a_appearance.revision));
                return nullptr;
            }

            auto* faceData = GetFaceRelatedData(race, a_appearance.female);
            npc->headRelatedData = new RE::TESNPC::HeadRelatedData();
            npc->faceData = RE::calloc<RE::TESNPC::FaceData>(1);
            npc->tintLayers = new RE::BSTArray<RE::TESNPC::Layer*>();

            // A remote player is one specific character, never a generic NPC.
            npc->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kUnique);
            npc->fullName = a_appearance.name;
            npc->race = race;
            npc->originalRace = race;
            // TESNPC::InitItemImpl completes these only for NPCs loaded from plugins:
            // an NPC without attack data takes its race's (BGSAttackDataForm::InitItem),
            // and one without a combat style gets the default object. Weapon draws
            // read the attack data map without a null check.
            npc->attackDataMap = race->attackDataMap;
            npc->combatStyle = GetDefaultCombatStyle();
            npc->actorData.actorBaseFlags.set(a_appearance.female, RE::ACTOR_BASE_DATA::Flag::kFemale);
            npc->weight = a_appearance.weight;
            npc->height = a_appearance.height;

            const auto headParts = ResolveHeadParts(a_playerID, a_appearance, *race, faceData);
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
            for (const auto& tint : a_appearance.tints)
            {
                const auto* asset = FindTintAsset(faceData, tint.index, tint.type);
                if (!asset)
                {
                    ++droppedTints;
                    continue;
                }
                auto* layer = RE::calloc<RE::TESNPC::Layer>(1);
                layer->tintColor = RE::Color{ tint.color.red, tint.color.green, tint.color.blue, 0 };
                layer->tintIndex = tint.index;
                layer->preset = FindTintPreset(*asset, tint.color);
                layer->interpolationValue
                    = static_cast<std::uint16_t>(std::lround(std::clamp(tint.alpha, 0.0F, 1.0F) * 100.0F));
                npc->tintLayers->push_back(layer);
            }

            logs::info(
                "Appearance sync built actor base: player={} revision={} base={:08X} name=\"{}\" race={:08X} female={} "
                "weight={:.1f} height={:.2f} headParts={}/{} hairColor={:08X} faceDetails={:08X} tints={}/{} "
                "droppedTints={} attackData={} combatStyle={:08X}",
                a_playerID, static_cast<unsigned long long>(a_appearance.revision), npc->GetFormID(), a_appearance.name,
                race->GetFormID(), a_appearance.female, a_appearance.weight, a_appearance.height, headParts.size(),
                a_appearance.headParts.size(), hairColor ? hairColor->GetFormID() : 0,
                faceDetails ? faceDetails->GetFormID() : 0, npc->tintLayers->size(), a_appearance.tints.size(),
                droppedTints, npc->attackDataMap != nullptr, npc->combatStyle ? npc->combatStyle->GetFormID() : 0);
            return npc;
        }

        // The look of a remote player whose appearance never arrived: the fallback
        // race's default head parts and skin tone.
        AppearanceData MakeDefaultAppearance()
        {
            AppearanceData appearance{ .race = AppearanceFormReference{ .plugin = std::string{ kFallbackRacePlugin },
                                           .localFormID = kFallbackRaceLocalFormID } };
            const auto* faceData = GetFaceRelatedData(LookupFallbackRace(), appearance.female);
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
                        appearance.headParts.push_back(AppearanceHeadPart{
                            .form = *reference, .type = static_cast<std::uint8_t>(headPart->type.underlying()) });
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

            // PrepareHeadPartForShaders (ID 26838) names the face geometry after the
            // face head part's editor ID.
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

        // PrepareHeadPartForShaders (ID 26838) gives a runtime-built face a blank tint
        // texture and renders tint masks into it only for the player base, or for
        // other NPCs when bUseFaceGenPreprocessedHeads is off. Remote actor bases get
        // neither, so the blank texture darkens the face. Queue the same render the
        // engine uses (ID 27040) from the remote actor base's tint layers.
        std::size_t QueueFaceTintRender(const RE::TESNPC& a_base, RE::NiSourceTexture& a_texture)
        {
            // A render job has 16 layer slots and does not check the mask count.
            constexpr std::size_t kMaximumRenderedTints = 16;
            using RenderTintMasks = bool(RE::BSTArray<RE::TintMask*>*, RE::NiTexture*);
            static REL::Relocation<RenderTintMasks> renderTintMasks{ REL::ID(27040) };

            auto* race = a_base.race;
            const auto* faceData = GetFaceRelatedData(race, a_base.IsFemale());
            std::array<RE::TintMask, kMaximumRenderedTints> masks{};
            RE::BSTArray<RE::TintMask*> maskList;
            if (a_base.tintLayers)
            {
                for (const auto* layer : *a_base.tintLayers)
                {
                    if (maskList.size() >= kMaximumRenderedTints)
                    {
                        break;
                    }
                    const auto* asset = layer ? FindTintAsset(faceData, layer->tintIndex) : nullptr;
                    if (!asset || layer->interpolationValue == 0)
                    {
                        continue;
                    }
                    auto& mask = masks[maskList.size()];
                    mask.texture = const_cast<RE::TESTexture*>(std::addressof(asset->texture.file));
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

    void SyncAppearance(const LocalFrame& a_local, float a_delta)
    {
        static float captureElapsed = kCaptureInterval;
        static float publishElapsed = kMinimumPublishInterval;
        static std::optional<AppearanceData> published;
        static std::optional<AppearanceData> candidate;
        static std::uint64_t revision = 0;
        static std::uint64_t publishedConnectionGeneration = 0;

        const float delta = std::isfinite(a_delta) ? (std::max)(a_delta, 0.0F) : 0.0F;
        captureElapsed += delta;
        publishElapsed += delta;

        if (!Network::IsConnected())
        {
            return;
        }
        if (const auto generation = Network::GetConnectionGeneration(); generation != publishedConnectionGeneration)
        {
            publishedConnectionGeneration = generation;
            // The server forgets a connection's appearance when it drops.
            if (published)
            {
                logs::info("Appearance sync republishing after connect: revision={} connectionGeneration={}",
                    static_cast<unsigned long long>(published->revision), static_cast<unsigned long long>(generation));
                Network::PublishLocalAppearance(*published);
                publishElapsed = 0.0F;
            }
        }

        if (captureElapsed < kCaptureInterval)
        {
            return;
        }
        captureElapsed = 0.0F;

        auto captured = CaptureLocalAppearance(a_local.player);
        if (!captured)
        {
            return;
        }
        if (published && published->HasSameLook(*captured))
        {
            candidate.reset();
            return;
        }
        // Require two identical captures so editor slider drags are not streamed.
        if (!candidate || !candidate->HasSameLook(*captured))
        {
            candidate = std::move(captured);
            return;
        }
        if (publishElapsed < kMinimumPublishInterval)
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
            logs::warn("Appearance sync skipped invalid local appearance: revision={}",
                static_cast<unsigned long long>(revision));
            candidate.reset();
            return;
        }

        logs::info(
            "Appearance sync publishing local appearance: revision={} name=\"{}\" race={}|{:06X} female={} "
            "headParts={} tints={}",
            static_cast<unsigned long long>(candidate->revision), candidate->name, candidate->race.plugin,
            candidate->race.localFormID, candidate->female, candidate->headParts.size(), candidate->tints.size());
        Network::PublishLocalAppearance(*candidate);
        published = std::move(candidate);
        candidate.reset();
        publishElapsed = 0.0F;
    }

    void AcceptRemoteAppearance(
        RemoteState& a_remote, const std::string& a_playerID, Multiplayer::PlayerAppearance&& a_appearance)
    {
        if (a_remote.latest && a_appearance.revision <= a_remote.latest->revision)
        {
            return;
        }

        logs::info(
            "Appearance sync accepted remote appearance: player={} revision={} name=\"{}\" race={}|{:06X} female={} "
            "headParts={} tints={}",
            a_playerID, static_cast<unsigned long long>(a_appearance.revision), a_appearance.name,
            a_appearance.race.plugin, a_appearance.race.localFormID, a_appearance.female, a_appearance.headParts.size(),
            a_appearance.tints.size());
        a_remote.latest = std::move(a_appearance);
    }

    void SyncAppearance(RemoteState& a_remote, float a_delta)
    {
        const float delta = std::isfinite(a_delta) ? (std::max)(a_delta, 0.0F) : 0.0F;
        if (!a_remote.latest)
        {
            a_remote.appearanceWaitElapsed += delta;
        }
        a_remote.respawnCooldown = (std::max)(a_remote.respawnCooldown - delta, 0.0F);
    }

    RE::TESNPC* GetSpawnBase(RemoteState& a_remote, const std::string& a_playerID)
    {
        if (!a_remote.latest && a_remote.appearanceWaitElapsed < kFirstAppearanceWaitSeconds)
        {
            return nullptr;
        }

        // Revision zero stands for the default look.
        const auto revision = a_remote.latest ? a_remote.latest->revision : 0;
        if ((!a_remote.base || a_remote.baseRevision != revision) && a_remote.failedRevision != revision)
        {
            auto* base = a_remote.latest ? CreateActorBase(a_playerID, *a_remote.latest)
                                         : CreateActorBase(a_playerID, MakeDefaultAppearance());
            if (base)
            {
                a_remote.base = base;
                a_remote.baseRevision = revision;
            }
            else
            {
                a_remote.failedRevision = revision;
            }
        }

        // A revision that could not be built keeps the previously built base.
        return a_remote.base;
    }

    void OnActorSpawned(RemoteState& a_remote, RE::TESNPC* a_spawnedBase)
    {
        // A failed build also counts as spawned for that revision, so the remote actor
        // is not retired every frame for an appearance that cannot be built.
        a_remote.spawnedRevision = a_remote.latest ? a_remote.latest->revision : 0;
        a_remote.spawnedBase = a_spawnedBase;
        a_remote.respawnCooldown = kMinimumRespawnInterval;
        a_remote.colorsApplied = false;
        a_remote.faceTintTexture.reset();
    }

    bool RequiresRespawn(const RemoteState& a_remote)
    {
        return a_remote.latest && a_remote.latest->revision != a_remote.spawnedRevision
            && a_remote.respawnCooldown <= 0.0F;
    }

    void ApplyActorPresentation(
        RemoteState& a_remote, const std::string& a_playerID, RE::Actor& a_actor, bool a_actorReady)
    {
        if (!a_actorReady || !a_actor.Is3DLoaded() || !a_remote.spawnedBase || a_remote.spawnedBase != a_remote.base
            || a_actor.GetActorBase() != a_remote.spawnedBase)
        {
            return;
        }

        if (!a_remote.colorsApplied)
        {
            a_remote.colorsApplied = true;
            a_actor.UpdateHairColor();
            a_actor.UpdateSkinColor();
            logs::info(
                "Appearance sync applied actor colors: player={} actor={:08X} base={:08X} revision={} faceNode={}",
                a_playerID, a_actor.GetHandle().native_handle(), a_remote.spawnedBase->GetFormID(),
                static_cast<unsigned long long>(a_remote.spawnedRevision), a_actor.GetFaceNodeSkinned() != nullptr);
        }

        // The face tint render is AE-only: its engine IDs are verified against 1.7.104.
        // A head rebuilt by a 3D reset gets a new blank texture, so render again.
        if (!REL::Module::IsAE())
        {
            return;
        }
        auto* tintTexture = FindFaceTintTexture(a_actor, *a_remote.spawnedBase);
        if (!tintTexture || tintTexture == a_remote.faceTintTexture.get())
        {
            return;
        }
        a_remote.faceTintTexture.reset(tintTexture);
        const auto renderedTints = QueueFaceTintRender(*a_remote.spawnedBase, *tintTexture);
        logs::info("Appearance sync queued actor face tint: player={} actor={:08X} base={:08X} revision={} tints={}/{}",
            a_playerID, a_actor.GetHandle().native_handle(), a_remote.spawnedBase->GetFormID(),
            static_cast<unsigned long long>(a_remote.spawnedRevision), renderedTints,
            a_remote.spawnedBase->tintLayers ? a_remote.spawnedBase->tintLayers->size() : 0);
    }
}
