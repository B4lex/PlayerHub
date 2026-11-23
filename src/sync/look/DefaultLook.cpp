#include "sync/look/DefaultLook.h"
#include "config/PluginConfig.h"
#include "config/SettingsSchema.h"
#include "forms/FormLookup.h"
#include "forms/ItemForms.h"
#include "forms/RaceForms.h"
#include "sync/look/LookPreset.h"
#include "text/AsciiCase.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <format>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace DefaultLook
{
    namespace
    {
        using Settings = Config::DefaultLookSettings;
        using Multiplayer::EquipmentHandKind;
        using Multiplayer::FormReference;

        const FormReference kTorch{ .plugin = "Skyrim.esm", .localFormID = 0x01D4EC };

        using Multiplayer::AppearanceDefaultFace;
        using HeadPartFlag = RE::BGSHeadPart::Flag;
        using HeadPartType = RE::BGSHeadPart::HeadPartType;

        Settings local;
        std::optional<AppearanceDefaultFace> defaultFace;
        std::shared_ptr<const Config::PluginConfig> resolvedConfig;
        std::uint64_t resolvedConfigRevision{ 0 };

        struct OutcomeRequest
        {
            std::string preset;
            FormReference race;
        };

        std::mutex outcomeLock;
        PresetOutcome latestOutcome;
        std::optional<OutcomeRequest> outcomeRequest;

        std::string_view FindUnusableReason(const FormReference& a_reference, const Forms::ItemFilter& a_filter)
        {
            auto* form = FormLookup::ResolveForm(a_reference);
            if (!form)
            {
                return "missing";
            }
            const auto traits = Forms::GetItemTraits(*form);
            return traits && a_filter.Matches(*traits) ? std::string_view{} : "does-not-fit";
        }

        FormReference ResolveConfigured(const FormReference& a_reference, const FormReference& a_default,
            std::string_view a_label, const Forms::ItemFilter& a_filter, std::uint32_t& a_unusable)
        {
            if (a_reference.IsEmpty())
            {
                return {};
            }
            const auto reason = FindUnusableReason(a_reference, a_filter);
            if (reason.empty())
            {
                return a_reference;
            }
            ++a_unusable;
            // The Anniversary Edition backpack default is missing from installs
            // without it.
            const bool useDefault = a_default != a_reference && FindUnusableReason(a_default, a_filter).empty();
            logs::warn("Default look item ignored [DefaultLook] {}: item={}|{:06X} reason={} using={}", a_label,
                a_reference.plugin, a_reference.localFormID, reason, useDefault ? "default" : "none");
            return useDefault ? a_default : FormReference{};
        }

        bool IsBaseGameItem(const FormReference& a_reference)
        {
            return Multiplayer::IsBaseGamePlugin(a_reference.plugin);
        }

        bool NeedsStandIn(const FormReference& a_reference)
        {
            return local.alwaysUse || !IsBaseGameItem(a_reference);
        }

        const Multiplayer::EquipmentHand* FindHandPiece(
            const Multiplayer::DefaultOutfit& a_outfit, const Multiplayer::EquipmentHand& a_entry)
        {
            const auto piece
                = std::ranges::find_if(a_outfit.hands, [&a_entry](const Multiplayer::EquipmentHand& a_piece) {
                      return a_piece.kind == a_entry.kind && a_piece.weaponType == a_entry.weaponType;
                  });
            return piece == a_outfit.hands.end() ? nullptr : &*piece;
        }

        void ReplaceWithOutfit(Multiplayer::PlayerEquipment& a_equipment)
        {
            const auto outfit = std::exchange(a_equipment.defaultOutfit, {});

            a_equipment.armor.clear();
            for (const auto& piece : outfit.armor)
            {
                auto* armor = FormLookup::ResolveForm<RE::TESObjectARMO>(piece.form);
                const auto slotMask = armor ? armor->GetSlotMask().underlying() : 0;
                if (slotMask != 0)
                {
                    a_equipment.armor.push_back(Multiplayer::EquipmentArmor{ .form = piece.form, .slotMask = slotMask });
                }
            }

            for (auto* hand : { &a_equipment.rightHand, &a_equipment.leftHand })
            {
                if (!*hand || (*hand)->kind == EquipmentHandKind::kSpell)
                {
                    continue;
                }
                if (const auto* piece = FindHandPiece(outfit, **hand))
                {
                    (*hand)->form = piece->form;
                }
                else
                {
                    hand->reset();
                }
            }
            a_equipment.ammo = outfit.ammo;
        }

        // The sex a head part is flagged for alone.
        std::optional<bool> GetHeadPartSex(const RE::BGSHeadPart& a_headPart)
        {
            const bool male = a_headPart.flags.all(HeadPartFlag::kMale);
            const bool female = a_headPart.flags.all(HeadPartFlag::kFemale);
            return male == female ? std::nullopt : std::optional{ female };
        }

        // SKEE applies a preset to the target's own sex, so the preset does not store
        // one. The face part's flag decides, then a majority of the other parts.
        std::optional<bool> FindPresetSex(std::span<RE::BGSHeadPart* const> a_headParts)
        {
            int femaleVotes = 0;
            for (const auto* headPart : a_headParts)
            {
                const auto sex = GetHeadPartSex(*headPart);
                if (sex && headPart->type == HeadPartType::kFace)
                {
                    return sex;
                }
                femaleVotes += !sex ? 0 : *sex ? 1 : -1;
            }
            return femaleVotes == 0 ? std::nullopt : std::optional{ femaleVotes > 0 };
        }

        const RE::TESRace::FaceRelatedData::TintAsset* FindTintAsset(
            const RE::TESRace::FaceRelatedData* a_faceData, std::string_view a_texture)
        {
            if (!a_faceData || !a_faceData->tintMasks || a_texture.empty())
            {
                return nullptr;
            }
            for (const auto* asset : *a_faceData->tintMasks)
            {
                if (asset && AsciiCase::Equals(asset->texture.file.textureName.c_str(), a_texture))
                {
                    return asset;
                }
            }
            return nullptr;
        }

        std::optional<Multiplayer::AppearanceHeadPart> MakeBaseGameHeadPart(const RE::BGSHeadPart* a_headPart)
        {
            const auto reference = FormLookup::MakeFormReference(a_headPart);
            if (!reference || !IsBaseGameItem(*reference))
            {
                return std::nullopt;
            }
            return Multiplayer::AppearanceHeadPart{ .form = *reference, .type = a_headPart->type.underlying() };
        }

        struct HeadPartCounts
        {
            std::uint32_t used{ 0 };
            std::uint32_t modded{ 0 };
            std::uint32_t wrongRace{ 0 };
            std::uint32_t wrongSex{ 0 };
            std::uint32_t duplicate{ 0 };
            std::uint32_t replaced{ 0 };
        };

        std::vector<RE::BGSHeadPart*> LoadHeadParts(const LookPreset::PresetFace& a_preset, std::uint32_t& a_missing)
        {
            std::vector<RE::BGSHeadPart*> headParts;
            for (const auto& reference : a_preset.headParts)
            {
                auto* headPart = FormLookup::ResolveForm<RE::BGSHeadPart>(reference);
                if (!headPart)
                {
                    ++a_missing;
                    logs::debug("Default look preset head part missing: part={}|{:06X}", reference.plugin,
                        reference.localFormID);
                    continue;
                }
                // Extra parts are children of their top-level part and are implied.
                if (!headPart->IsExtraPart())
                {
                    headParts.push_back(headPart);
                }
            }
            return headParts;
        }

        void ResolveHeadParts(std::span<RE::BGSHeadPart* const> a_headParts, RE::TESRace& a_race,
            bool a_characterFemale, bool a_log, AppearanceDefaultFace& a_face, HeadPartCounts& a_counts)
        {
            const auto sex = FindPresetSex(a_headParts);
            if (!sex && a_log)
            {
                logs::warn("Default look preset sex undecided; keeping the character's: female={}", a_characterFemale);
            }
            a_face.female = sex.value_or(a_characterFemale);

            // A part left out falls back to the race's default part of its type.
            std::uint32_t fallbackTypes = 0;
            std::uint32_t usedTypes = 0;
            for (auto* headPart : a_headParts)
            {
                const auto type = headPart->type.underlying();
                const auto reference = FormLookup::MakeFormReference(headPart);
                const auto* reason = "";
                if (!reference || !IsBaseGameItem(*reference) || type > AppearanceDefaultFace::kMaximumHeadPartType)
                {
                    ++a_counts.modded;
                    reason = "modded";
                }
                else if (headPart->validRaces && !headPart->validRaces->HasForm(&a_race))
                {
                    ++a_counts.wrongRace;
                    reason = "race";
                }
                else if (const auto partSex = GetHeadPartSex(*headPart); partSex && *partSex != a_face.female)
                {
                    ++a_counts.wrongSex;
                    reason = "sex";
                }
                else if ((usedTypes & (1U << type)) != 0)
                {
                    ++a_counts.duplicate;
                    logs::debug("Default look preset head part duplicates its type: part={:08X} type={}",
                        headPart->GetFormID(), type);
                    continue;
                }
                else
                {
                    usedTypes |= 1U << type;
                    a_face.headParts.push_back(Multiplayer::AppearanceHeadPart{ .form = *reference, .type = type });
                    continue;
                }
                logs::debug("Default look preset head part left out: part={:08X} type={} reason={}",
                    headPart->GetFormID(), type, reason);
                if (type <= AppearanceDefaultFace::kMaximumHeadPartType)
                {
                    fallbackTypes |= 1U << type;
                }
            }

            const auto* faceData = Forms::GetFaceRelatedData(&a_race, a_face.female);
            for (std::uint32_t type = 0; type <= AppearanceDefaultFace::kMaximumHeadPartType; ++type)
            {
                if ((fallbackTypes & ~usedTypes & (1U << type)) == 0)
                {
                    continue;
                }
                if (const auto fallback = MakeBaseGameHeadPart(Forms::FindDefaultHeadPart(faceData, type)))
                {
                    a_face.headParts.push_back(*fallback);
                    ++a_counts.replaced;
                }
            }
            a_counts.used = static_cast<std::uint32_t>(a_face.headParts.size());
        }

        std::uint32_t ResolveTints(
            const LookPreset::PresetFace& a_preset, RE::TESRace& a_race, AppearanceDefaultFace& a_face)
        {
            using SkinTone = RE::TESRace::FaceRelatedData::TintAsset::TintLayer::SkinTone;
            const auto* faceData = Forms::GetFaceRelatedData(&a_race, a_face.female);
            bool skinTone = false;
            std::uint32_t dropped = 0;
            for (const auto& tint : a_preset.tints)
            {
                const auto* asset = FindTintAsset(faceData, tint.texture);
                const std::uint8_t type = asset ? asset->texture.skinTone.underlying() : std::uint8_t{ 0 };
                if (!asset || type > Multiplayer::PlayerAppearance::kMaximumTintType
                    || a_face.tints.size() >= AppearanceDefaultFace::kMaximumTints)
                {
                    ++dropped;
                    logs::debug("Default look preset tint left out: texture=\"{}\" asset={}", tint.texture,
                        asset != nullptr);
                    continue;
                }
                if (type == std::to_underlying(SkinTone::kSkinTone) && !skinTone)
                {
                    skinTone = true;
                    a_face.bodyTint = tint.color;
                }
                a_face.tints.push_back(Multiplayer::AppearanceTint{
                    .index = asset->texture.index, .type = type, .color = tint.color, .alpha = tint.alpha });
            }

            if (!skinTone && faceData && faceData->tintMasks)
            {
                for (const auto* asset : *faceData->tintMasks)
                {
                    const auto* color = asset ? asset->texture.presetDefault : nullptr;
                    if (color && asset->texture.skinTone == SkinTone::kSkinTone)
                    {
                        a_face.bodyTint = Multiplayer::AppearanceColor{
                            .red = color->color.red, .green = color->color.green, .blue = color->color.blue };
                        break;
                    }
                }
            }
            return dropped;
        }

        struct PresetResolution
        {
            std::optional<AppearanceDefaultFace> face;
            PresetOutcome outcome;
        };

        // a_race keys the outcome; a_usableRace is its usable form, which the face is resolved for.
        PresetResolution ResolvePreset(
            const std::string& a_preset, const FormReference& a_race, const FormReference& a_usableRace, bool a_log)
        {
            PresetResolution resolution{ .outcome = PresetOutcome{ .preset = a_preset, .race = a_race } };
            auto& outcome = resolution.outcome;
            if (a_preset.empty())
            {
                return resolution;
            }
            auto* race = FormLookup::ResolveForm<RE::TESRace>(a_usableRace);
            const auto loaded = race ? LookPreset::Load(a_preset)
                                     : std::expected<LookPreset::PresetFace, std::string>{ std::unexpect, "no-race" };
            if (!loaded)
            {
                outcome.unreadable = true;
                if (a_log)
                {
                    logs::warn("Default look preset unusable: preset=\"{}\" reason={}", a_preset, loaded.error());
                }
                return resolution;
            }
            const auto& preset = *loaded;

            HeadPartCounts parts;
            const auto headParts = LoadHeadParts(preset, parts.modded);

            const auto* player = RE::PlayerCharacter::GetSingleton();
            const auto* playerBase = player ? player->GetActorBase() : nullptr;
            AppearanceDefaultFace face{ .faceMorphs = preset.faceMorphs, .faceParts = preset.faceParts };
            ResolveHeadParts(headParts, *race, playerBase && playerBase->IsFemale(), a_log, face, parts);
            face.hairColor = preset.hairColor;

            std::uint32_t moddedDetails = 0;
            if (!preset.faceDetails.IsEmpty())
            {
                if (IsBaseGameItem(preset.faceDetails) && FormLookup::ResolveForm<RE::BGSTextureSet>(preset.faceDetails))
                {
                    face.faceDetails = preset.faceDetails;
                }
                else
                {
                    moddedDetails = 1;
                }
            }
            const auto droppedTints = ResolveTints(preset, *race, face);

            outcome.moddedParts = parts.modded + moddedDetails;
            outcome.unfitParts = parts.wrongRace + parts.wrongSex;
            outcome.droppedTints = droppedTints;
            if (a_log)
            {
                logs::info("Default look preset loaded: preset=\"{}\" race={:08X} female={} "
                           "headParts={}/{} modded={} wrongRace={} wrongSex={} duplicate={} replaced={} "
                           "moddedFaceDetails={} tints={}/{} droppedTints={} unreadableEntries={} raceMenuData={}",
                    a_preset, race->GetFormID(), face.female, parts.used, preset.headParts.size(), parts.modded,
                    parts.wrongRace, parts.wrongSex, parts.duplicate, parts.replaced, moddedDetails, face.tints.size(),
                    preset.tints.size(), droppedTints, preset.unreadableEntries, preset.raceMenuData);
            }
            resolution.face = std::move(face);
            return resolution;
        }

        std::size_t GetSlotRank(std::uint32_t a_slotMask)
        {
            for (std::size_t rank = 0; rank < Settings::kArmorSlotCount; ++rank)
            {
                if ((a_slotMask & Settings::kArmorSlotMasks[Settings::kArmorSlotPrecedence[rank]]) != 0)
                {
                    return rank;
                }
            }
            return Settings::kArmorSlotCount;
        }
    }

    void Initialize()
    {
        resolvedConfigRevision = Config::GetRevision();
        resolvedConfig = Config::Get();
        const auto& settings = resolvedConfig->defaultLook;
        const auto& defaults = Config::GetDefaultConfig().defaultLook;
        local = {};
        local.alwaysUse = settings.alwaysUse;

        std::uint32_t unusable = 0;
        local.race = ResolveConfigured(settings.race, defaults.race, "Race", Config::kLookRaceFilter, unusable);
        for (std::size_t slot = 0; slot < settings.armor.size(); ++slot)
        {
            local.armor[slot] = ResolveConfigured(settings.armor[slot], defaults.armor[slot],
                Config::GetLookArmorKey(slot), Config::GetLookArmorFilter(slot), unusable);
        }
        for (std::size_t type = Multiplayer::PlayerEquipment::kMinimumWeaponType; type < settings.weapons.size();
            ++type)
        {
            local.weapons[type] = ResolveConfigured(settings.weapons[type], defaults.weapons[type],
                Config::GetLookWeaponKey(type), Config::GetLookWeaponFilter(type), unusable);
        }
        local.arrow = ResolveConfigured(settings.arrow, defaults.arrow, "Arrow", Config::kLookArrowFilter, unusable);
        local.bolt = ResolveConfigured(settings.bolt, defaults.bolt, "Bolt", Config::kLookBoltFilter, unusable);
        local.preset = settings.preset;

        auto resolution = ResolvePreset(settings.preset, settings.race, local.race, true);
        defaultFace = std::move(resolution.face);
        {
            std::scoped_lock lock{ outcomeLock };
            latestOutcome = std::move(resolution.outcome);
        }

        logs::info("Default look initialized: unusableItems={} preset={}", unusable, defaultFace.has_value());
    }

    void Refresh()
    {
        const auto revision = Config::GetRevision();
        if (!resolvedConfig || revision == resolvedConfigRevision)
        {
            return;
        }
        if (Config::Get()->defaultLook == resolvedConfig->defaultLook)
        {
            resolvedConfigRevision = revision;
            return;
        }
        logs::debug("Default look settings changed; resolving the configured items again");
        Initialize();
    }

    void ServeRequests()
    {
        if (!resolvedConfig)
        {
            return;
        }
        std::optional<OutcomeRequest> request;
        {
            std::scoped_lock lock{ outcomeLock };
            request = std::exchange(outcomeRequest, std::nullopt);
        }
        if (!request)
        {
            return;
        }
        const auto usableRace = FindUnusableReason(request->race, Config::kLookRaceFilter).empty()
            ? request->race
            : Config::GetDefaultConfig().defaultLook.race;
        auto resolution = ResolvePreset(request->preset, request->race, usableRace, false);
        std::scoped_lock lock{ outcomeLock };
        latestOutcome = std::move(resolution.outcome);
    }

    void Select(Multiplayer::PlayerEquipment& a_equipment)
    {
        auto& outfit = a_equipment.defaultOutfit;
        outfit = {};

        std::uint32_t standInSlots = 0;
        for (const auto& entry : a_equipment.armor)
        {
            if (NeedsStandIn(entry.form))
            {
                standInSlots |= entry.slotMask;
            }
        }
        for (const auto slot : Settings::kArmorSlotPrecedence)
        {
            const auto slotMask = Settings::kArmorSlotMasks[slot];
            if ((standInSlots & slotMask) != 0 && !local.armor[slot].IsEmpty())
            {
                outfit.armor.push_back(Multiplayer::EquipmentArmor{ .form = local.armor[slot], .slotMask = slotMask });
            }
        }

        const auto appendHand = [&outfit](const std::optional<Multiplayer::EquipmentHand>& a_hand) {
            if (!a_hand || a_hand->kind == EquipmentHandKind::kSpell || !NeedsStandIn(a_hand->form))
            {
                return;
            }
            const bool torch = a_hand->kind == EquipmentHandKind::kTorch;
            if (!torch && a_hand->weaponType >= local.weapons.size())
            {
                return;
            }
            const auto& form = torch ? kTorch : local.weapons[a_hand->weaponType];
            const bool duplicate
                = std::ranges::any_of(outfit.hands, [&a_hand](const Multiplayer::EquipmentHand& a_piece) {
                      return a_piece.kind == a_hand->kind && a_piece.weaponType == a_hand->weaponType;
                  });
            if (!form.IsEmpty() && !duplicate)
            {
                outfit.hands.push_back(
                    Multiplayer::EquipmentHand{ .form = form, .kind = a_hand->kind, .weaponType = a_hand->weaponType });
            }
        };
        appendHand(a_equipment.rightHand);
        appendHand(a_equipment.leftHand);

        if (!a_equipment.ammo.IsEmpty() && NeedsStandIn(a_equipment.ammo))
        {
            auto* ammo = FormLookup::ResolveForm<RE::TESAmmo>(a_equipment.ammo);
            outfit.ammo = ammo && ammo->IsBolt() ? local.bolt : local.arrow;
        }

        if (local.alwaysUse)
        {
            ReplaceWithOutfit(a_equipment);
        }
    }

    std::optional<Multiplayer::FormReference> GetAlwaysUseRace()
    {
        return local.alwaysUse && !local.race.IsEmpty() ? std::optional{ local.race } : std::nullopt;
    }

    const std::optional<Multiplayer::AppearanceDefaultFace>& GetDefaultFace()
    {
        return defaultFace;
    }

    void RequestPresetOutcome(std::string_view a_preset, const Multiplayer::FormReference& a_race)
    {
        std::scoped_lock lock{ outcomeLock };
        if (latestOutcome.preset == a_preset && latestOutcome.race == a_race)
        {
            outcomeRequest.reset();
            return;
        }
        outcomeRequest = OutcomeRequest{ .preset = std::string{ a_preset }, .race = a_race };
    }

    PresetOutcome GetPresetOutcome()
    {
        std::scoped_lock lock{ outcomeLock };
        return latestOutcome;
    }

    void SelectRace(Multiplayer::PlayerAppearance& a_appearance)
    {
        a_appearance.defaultRace.reset();
        a_appearance.defaultFace.reset();
        if (!IsBaseGameItem(a_appearance.race) && !local.race.IsEmpty())
        {
            a_appearance.defaultRace = local.race;
            a_appearance.defaultFace = defaultFace;
        }
    }

    std::array<RE::TESObjectARMO*, Multiplayer::DefaultOutfit::kMaximumArmor> SelectArmor(
        const Multiplayer::DefaultOutfit& a_outfit, std::span<RE::TESObjectARMO* const> a_resolved,
        std::uint32_t a_missingSlots, std::vector<RE::TESObjectARMO*>& a_armor)
    {
        struct Candidate
        {
            std::size_t rank{ 0 };
            std::size_t piece{ 0 };
            RE::TESObjectARMO* armor{ nullptr };
        };
        constexpr auto kNoPiece = (std::numeric_limits<std::size_t>::max)();

        std::vector<Candidate> candidates;
        candidates.reserve(a_resolved.size() + a_outfit.armor.size());
        for (auto* armor : a_resolved)
        {
            candidates.push_back(Candidate{ .rank = GetSlotRank(armor->GetSlotMask().underlying()),
                .piece = kNoPiece,
                .armor = armor });
        }
        const auto pieceCount = (std::min)(a_outfit.armor.size(), Multiplayer::DefaultOutfit::kMaximumArmor);
        for (std::size_t index = 0; index < pieceCount; ++index)
        {
            const auto& piece = a_outfit.armor[index];
            if ((piece.slotMask & a_missingSlots) == 0)
            {
                continue;
            }
            auto* armor = FormLookup::ResolveForm<RE::TESObjectARMO>(piece.form);
            if (armor && armor->GetSlotMask().underlying() != 0)
            {
                candidates.push_back(Candidate{ .rank = GetSlotRank(piece.slotMask), .piece = index, .armor = armor });
            }
        }
        std::ranges::stable_sort(candidates, [](const Candidate& a_left, const Candidate& a_right) {
            return a_left.rank != a_right.rank ? a_left.rank < a_right.rank
                                               : (a_left.piece == kNoPiece && a_right.piece != kNoPiece);
        });

        std::array<RE::TESObjectARMO*, Multiplayer::DefaultOutfit::kMaximumArmor> standIns{};
        a_armor.clear();
        std::uint32_t occupiedSlots = 0;
        for (const auto& candidate : candidates)
        {
            const bool standIn = candidate.piece != kNoPiece;
            if (standIn && (a_outfit.armor[candidate.piece].slotMask & a_missingSlots & ~occupiedSlots) == 0)
            {
                continue;
            }
            // Equipping over an occupied slot would unequip the item taken there.
            const auto armorSlots = candidate.armor->GetSlotMask().underlying();
            if ((armorSlots & occupiedSlots) != 0)
            {
                continue;
            }
            occupiedSlots |= armorSlots;
            a_armor.push_back(candidate.armor);
            if (standIn)
            {
                standIns[candidate.piece] = candidate.armor;
            }
        }
        return standIns;
    }

    RE::TESBoundObject* FindHand(const Multiplayer::DefaultOutfit& a_outfit, const Multiplayer::EquipmentHand& a_entry)
    {
        if (a_entry.kind == EquipmentHandKind::kSpell)
        {
            return nullptr;
        }
        const auto* piece = FindHandPiece(a_outfit, a_entry);
        if (!piece)
        {
            return nullptr;
        }
        if (a_entry.kind == EquipmentHandKind::kTorch)
        {
            auto* light = FormLookup::ResolveForm<RE::TESObjectLIGH>(piece->form);
            return light && light->CanBeCarried() ? light : nullptr;
        }
        auto* weapon = FormLookup::ResolveForm<RE::TESObjectWEAP>(piece->form);
        return weapon && static_cast<std::uint8_t>(weapon->GetWeaponType()) == a_entry.weaponType ? weapon : nullptr;
    }

    RE::TESAmmo* FindAmmo(const Multiplayer::DefaultOutfit& a_outfit)
    {
        return FormLookup::ResolveForm<RE::TESAmmo>(a_outfit.ammo);
    }
}
