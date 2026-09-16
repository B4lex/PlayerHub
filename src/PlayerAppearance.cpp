#include "PlayerAppearance.h"
#include "SocketIOClient.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace PlayerAppearance
{
	namespace
	{
		constexpr float kCaptureInterval = 1.0F;
		constexpr float kMinimumPublishInterval = 3.0F;
		constexpr float kFirstAppearanceWaitSeconds = 3.0F;
		constexpr float kMinimumRespawnInterval = 2.0F;
		constexpr std::uint32_t kLightPluginLocalFormIDMask = 0x00000FFF;
		constexpr std::uint32_t kPluginLocalFormIDMask = 0x00FFFFFF;
		// Vanilla fallback when the source race is unavailable locally.
		constexpr std::string_view kFallbackRacePlugin = "Skyrim.esm";
		constexpr RE::FormID kFallbackRaceLocalFormID = 0x00013746;  // NordRace

		using Multiplayer::AppearanceColor;
		using Multiplayer::AppearanceFormReference;
		using Multiplayer::AppearanceHeadPart;
		using Multiplayer::AppearanceTint;
		using AppearanceData = Multiplayer::PlayerAppearance;
		using HeadPartType = RE::BGSHeadPart::HeadPartType;
		using FaceRelatedData = RE::TESRace::FaceRelatedData;
		using TintAsset = FaceRelatedData::TintAsset;

		constexpr AppearanceColor ToAppearanceColor(const RE::Color& a_color) noexcept
		{
			return AppearanceColor{
				.red = a_color.red,
				.green = a_color.green,
				.blue = a_color.blue
			};
		}

		bool EqualsIgnoreCase(std::string_view a_first, std::string_view a_second)
		{
			return std::ranges::equal(a_first, a_second, [](char a_left, char a_right) {
				return std::tolower(static_cast<unsigned char>(a_left)) ==
					std::tolower(static_cast<unsigned char>(a_right));
			});
		}

		bool IsPlayableRace(const RE::TESRace* a_race)
		{
			return a_race && a_race->data.flags.all(RE::RACE_DATA::Flag::kPlayable);
		}

		FaceRelatedData* GetFaceRelatedData(RE::TESRace* a_race, bool a_female)
		{
			return a_race ?
				a_race->faceRelatedData[a_female ? RE::SEXES::kFemale : RE::SEXES::kMale] :
				nullptr;
		}

		std::optional<AppearanceFormReference> MakeFormReference(const RE::TESForm* a_form)
		{
			if (!a_form || a_form->IsDynamicForm()) {
				return std::nullopt;
			}

			const auto* file = a_form->GetFile(0);
			if (!file) {
				return std::nullopt;
			}

			const auto fileName = file->GetFilename();
			if (fileName.empty() ||
				fileName.size() > AppearanceData::kMaximumPluginNameLength) {
				return std::nullopt;
			}

			return AppearanceFormReference{
				.plugin = std::string{ fileName },
				.localFormID = a_form->GetFormID() &
					(file->IsLight() ? kLightPluginLocalFormIDMask : kPluginLocalFormIDMask)
			};
		}

		template <class T>
		T* ResolveForm(const AppearanceFormReference& a_reference)
		{
			if (a_reference.IsEmpty()) {
				return nullptr;
			}

			auto* dataHandler = RE::TESDataHandler::GetSingleton();
			auto* form = dataHandler ?
				dataHandler->LookupForm(a_reference.localFormID, a_reference.plugin) :
				nullptr;
			return form ? form->As<T>() : nullptr;
		}

		const TintAsset* FindTintAsset(
			const FaceRelatedData* a_faceData,
			const RE::TintMask& a_mask)
		{
			if (!a_faceData || !a_faceData->tintMasks || !a_mask.texture) {
				return nullptr;
			}

			const std::string_view maskPath{ a_mask.texture->textureName.c_str() };
			for (const auto* asset : *a_faceData->tintMasks) {
				if (asset && &asset->texture.file == a_mask.texture) {
					return asset;
				}
			}
			if (maskPath.empty()) {
				return nullptr;
			}
			for (const auto* asset : *a_faceData->tintMasks) {
				if (asset &&
					EqualsIgnoreCase(asset->texture.file.textureName.c_str(), maskPath)) {
					return asset;
				}
			}
			return nullptr;
		}

		const TintAsset* FindTintAsset(
			const FaceRelatedData* a_faceData,
			std::uint16_t a_index,
			std::uint8_t a_type)
		{
			if (!a_faceData || !a_faceData->tintMasks) {
				return nullptr;
			}

			for (const auto* asset : *a_faceData->tintMasks) {
				if (asset && asset->texture.index == a_index &&
					asset->texture.skinTone.underlying() == a_type) {
					return asset;
				}
			}
			return nullptr;
		}

		// Logs a capture failure only when its reason changes, so a persistent
		// failure does not flood the log once per second.
		void LogCaptureFailure(std::string_view a_reason)
		{
			static std::string lastReason;
			if (lastReason == a_reason) {
				return;
			}
			lastReason = a_reason;
			logs::warn("Appearance sync could not capture local appearance: {}", a_reason);
		}

		std::optional<AppearanceData> CaptureLocalAppearance(RE::PlayerCharacter& a_player)
		{
			auto* base = a_player.GetActorBase();
			if (!base) {
				LogCaptureFailure("player has no actor base");
				return std::nullopt;
			}

			// Transformation races replace the skeleton and behavior graph. Keep the
			// last playable appearance instead of publishing a beast form.
			auto* race = base->race;
			if (!IsPlayableRace(race)) {
				race = IsPlayableRace(base->originalRace) ? base->originalRace : nullptr;
			}
			const auto raceReference = MakeFormReference(race);
			if (!raceReference) {
				const auto* file = race ? race->GetFile(0) : nullptr;
				LogCaptureFailure(std::format(
					"race is not referenceable: race={:08X} playable={} originalRace={:08X} originalPlayable={} dynamic={} file={}",
					base->race ? base->race->GetFormID() : 0,
					IsPlayableRace(base->race),
					base->originalRace ? base->originalRace->GetFormID() : 0,
					IsPlayableRace(base->originalRace),
					race && race->IsDynamicForm(),
					file ? file->GetFilename() : std::string_view{ "<none>" }));
				return std::nullopt;
			}

			AppearanceData appearance{
				.race = *raceReference,
				.female = base->IsFemale(),
				.weight = std::clamp(base->weight, 0.0F, AppearanceData::kMaximumWeight),
				.height = std::clamp(
					base->height,
					AppearanceData::kMinimumHeight,
					AppearanceData::kMaximumHeight)
			};

			if (base->headParts) {
				for (std::int8_t i = 0; i < base->numHeadParts; ++i) {
					auto* headPart = base->headParts[i];
					// Extra parts are children of their top-level part and are implied.
					if (!headPart || headPart->IsExtraPart()) {
						continue;
					}
					const auto reference = MakeFormReference(headPart);
					if (!reference ||
						appearance.headParts.size() >= AppearanceData::kMaximumHeadParts) {
						continue;
					}
					appearance.headParts.push_back(AppearanceHeadPart{
						.form = *reference,
						.type = static_cast<std::uint8_t>(headPart->type.underlying())
					});
				}
			}

			if (base->headRelatedData) {
				if (const auto* hairColor = base->headRelatedData->hairColor) {
					appearance.hairColor = ToAppearanceColor(hairColor->color);
				}
				if (const auto faceDetails =
						MakeFormReference(base->headRelatedData->faceDetails)) {
					appearance.faceDetails = *faceDetails;
				}
			}

			if (base->faceData) {
				for (std::size_t i = 0; i < AppearanceData::kFaceMorphCount; ++i) {
					const auto morph = base->faceData->morphs[i];
					appearance.faceMorphs[i] = std::isfinite(morph) ? morph : 0.0F;
				}
				for (std::size_t i = 0; i < AppearanceData::kFacePartCount; ++i) {
					appearance.faceParts[i] = base->faceData->parts[i];
				}
			}

			appearance.bodyTint = ToAppearanceColor(base->bodyTintColor);
			// The player's face tints live on PlayerCharacter rather than TESNPC. The
			// runtime-data layout is only known for flat Skyrim.
			if (!REL::Module::IsVR()) {
				const auto* faceData = GetFaceRelatedData(race, appearance.female);
				const auto& tintMasks = a_player.GetPlayerRuntimeData().tintMasks;
				for (const auto* mask : tintMasks) {
					if (!mask) {
						continue;
					}
					const auto type = static_cast<std::uint8_t>(mask->type.underlying());
					if (type == static_cast<std::uint8_t>(RE::TintMask::Type::kSkinTone) &&
						appearance.bodyTint == AppearanceColor{}) {
						appearance.bodyTint = ToAppearanceColor(mask->color);
					}
					if (!std::isfinite(mask->alpha) || mask->alpha <= 0.0F ||
						type > AppearanceData::kMaximumTintType ||
						appearance.tints.size() >= AppearanceData::kMaximumTints) {
						continue;
					}
					const auto* asset = FindTintAsset(faceData, *mask);
					if (!asset) {
						continue;
					}
					appearance.tints.push_back(AppearanceTint{
						.index = asset->texture.index,
						.type = type,
						.color = ToAppearanceColor(mask->color),
						.alpha = (std::min)(mask->alpha, 1.0F)
					});
				}
			}

			return appearance;
		}

		bool AssignDynamicFormID(RE::TESForm& a_form)
		{
			if (a_form.GetFormID() != 0) {
				return true;
			}

			auto* dataHandler = RE::TESDataHandler::GetSingleton();
			const auto formID = dataHandler ? dataHandler->GetNextID() : 0;
			if (formID == 0) {
				return false;
			}
			a_form.SetFormID(formID, false);
			return a_form.GetFormID() == formID;
		}

		RE::TESRace* ResolveRace(
			const std::string& a_playerID,
			const AppearanceData& a_appearance)
		{
			auto* race = ResolveForm<RE::TESRace>(a_appearance.race);
			if (IsPlayableRace(race)) {
				return race;
			}

			logs::warn(
				"Appearance sync falling back to NordRace: player={} revision={} race={}|{:06X} resolved={}",
				a_playerID,
				static_cast<unsigned long long>(a_appearance.revision),
				a_appearance.race.plugin,
				a_appearance.race.localFormID,
				race != nullptr);
			auto* dataHandler = RE::TESDataHandler::GetSingleton();
			return dataHandler ?
				dataHandler->LookupForm<RE::TESRace>(
					kFallbackRaceLocalFormID,
					kFallbackRacePlugin) :
				nullptr;
		}

		RE::BGSHeadPart* FindDefaultHeadPart(FaceRelatedData* a_faceData, std::uint8_t a_type)
		{
			if (!a_faceData || !a_faceData->headParts) {
				return nullptr;
			}
			for (auto* headPart : *a_faceData->headParts) {
				if (headPart && headPart->type.underlying() == a_type) {
					return headPart;
				}
			}
			return nullptr;
		}

		std::vector<RE::BGSHeadPart*> ResolveHeadParts(
			const std::string& a_playerID,
			const AppearanceData& a_appearance,
			RE::TESRace& a_race,
			FaceRelatedData* a_faceData)
		{
			std::vector<RE::BGSHeadPart*> headParts;
			const auto addUnique = [&headParts](RE::BGSHeadPart* a_headPart) {
				if (a_headPart && std::ranges::find(headParts, a_headPart) == headParts.end()) {
					headParts.push_back(a_headPart);
				}
			};

			for (const auto& reference : a_appearance.headParts) {
				auto* headPart = ResolveForm<RE::BGSHeadPart>(reference.form);
				const bool usable = headPart &&
					headPart->type.underlying() == reference.type &&
					(!headPart->validRaces || headPart->validRaces->HasForm(&a_race));
				if (usable) {
					addUnique(headPart);
					continue;
				}

				auto* fallback = FindDefaultHeadPart(a_faceData, reference.type);
				logs::warn(
					"Appearance sync replaced unavailable head part: player={} revision={} part={}|{:06X} type={} fallback={:08X}",
					a_playerID,
					static_cast<unsigned long long>(a_appearance.revision),
					reference.form.plugin,
					reference.form.localFormID,
					reference.type,
					fallback ? fallback->GetFormID() : 0);
				addUnique(fallback);
			}

			const auto faceType = static_cast<std::uint8_t>(HeadPartType::kFace);
			const bool hasFace = std::ranges::any_of(headParts, [faceType](const auto* a_part) {
				return a_part->type.underlying() == faceType;
			});
			if (!hasFace) {
				addUnique(FindDefaultHeadPart(a_faceData, faceType));
			}
			return headParts;
		}

		RE::BGSColorForm* CreateHairColor(const AppearanceColor& a_color)
		{
			auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::BGSColorForm>();
			auto* colorForm = factory ? factory->Create() : nullptr;
			if (!colorForm || !AssignDynamicFormID(*colorForm)) {
				return nullptr;
			}
			colorForm->color = RE::Color{ a_color.red, a_color.green, a_color.blue, 0 };
			return colorForm;
		}

		std::uint16_t FindTintPreset(const TintAsset& a_asset, const AppearanceColor& a_color)
		{
			const auto& colors = a_asset.presets.colors;
			for (std::uint32_t i = 0; i < colors.size(); ++i) {
				if (colors[i] && ToAppearanceColor(colors[i]->color) == a_color) {
					return static_cast<std::uint16_t>(i);
				}
			}
			return 0;
		}

		RE::TESNPC* CreatePuppetBase(
			const std::string& a_playerID,
			const AppearanceData& a_appearance,
			RE::TESNPC& a_localPlayerBase)
		{
			auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESNPC>();
			auto* npc = factory ? factory->Create() : nullptr;
			if (!npc || !AssignDynamicFormID(*npc)) {
				logs::error(
					"Appearance sync could not create an actor base: player={} revision={}",
					a_playerID,
					static_cast<unsigned long long>(a_appearance.revision));
				return nullptr;
			}

			const auto formID = npc->GetFormID();
			// Copy class, AI data, and factions from the player base so puppets keep
			// the same actor policy they had before appearance synchronization.
			npc->Copy(&a_localPlayerBase);
			if (npc->GetFormID() != formID) {
				logs::critical(
					"Appearance sync actor base changed form ID during Copy: player={} expected={:08X} actual={:08X}",
					a_playerID,
					formID,
					npc->GetFormID());
				return nullptr;
			}

			auto* race = ResolveRace(a_playerID, a_appearance);
			auto* faceData = GetFaceRelatedData(race, a_appearance.female);
			if (!race) {
				logs::error(
					"Appearance sync could not resolve any race: player={} revision={}",
					a_playerID,
					static_cast<unsigned long long>(a_appearance.revision));
				return nullptr;
			}

			// Copy may share appearance storage with the player base. Replace every
			// pointer before writing so the local player can never be modified.
			npc->faceNPC = nullptr;
			npc->headRelatedData = new RE::TESNPC::HeadRelatedData();
			npc->faceData = RE::calloc<RE::TESNPC::FaceData>(1);
			npc->tintLayers = new RE::BSTArray<RE::TESNPC::Layer*>();
			npc->headParts = nullptr;
			npc->numHeadParts = 0;

			npc->race = race;
			npc->originalRace = race;
			npc->actorData.actorBaseFlags.set(
				a_appearance.female,
				RE::ACTOR_BASE_DATA::Flag::kFemale);
			npc->weight = a_appearance.weight;
			npc->height = a_appearance.height;

			const auto headParts = ResolveHeadParts(a_playerID, a_appearance, *race, faceData);
			if (!headParts.empty()) {
				npc->headParts = RE::calloc<RE::BGSHeadPart*>(headParts.size());
				std::ranges::copy(headParts, npc->headParts);
				npc->numHeadParts = static_cast<std::int8_t>(headParts.size());
			}

			RE::BGSColorForm* hairColor = nullptr;
			if (a_appearance.hairColor) {
				hairColor = CreateHairColor(*a_appearance.hairColor);
			}
			if (!hairColor && faceData) {
				hairColor = faceData->defaultHairColor;
			}
			npc->headRelatedData->hairColor = hairColor;

			auto* faceDetails = ResolveForm<RE::BGSTextureSet>(a_appearance.faceDetails);
			if (!faceDetails && faceData) {
				faceDetails = faceData->defaultFaceDetailsTextureSet;
			}
			npc->headRelatedData->faceDetails = faceDetails;

			std::ranges::copy(a_appearance.faceMorphs, npc->faceData->morphs);
			std::ranges::copy(a_appearance.faceParts, npc->faceData->parts);
			npc->bodyTintColor.red = a_appearance.bodyTint.red;
			npc->bodyTintColor.green = a_appearance.bodyTint.green;
			npc->bodyTintColor.blue = a_appearance.bodyTint.blue;

			std::size_t droppedTints = 0;
			for (const auto& tint : a_appearance.tints) {
				const auto* asset = FindTintAsset(faceData, tint.index, tint.type);
				if (!asset) {
					++droppedTints;
					continue;
				}
				auto* layer = RE::calloc<RE::TESNPC::Layer>(1);
				layer->tintColor = RE::Color{ tint.color.red, tint.color.green, tint.color.blue, 0 };
				layer->tintIndex = tint.index;
				layer->preset = FindTintPreset(*asset, tint.color);
				layer->interpolationValue = static_cast<std::uint16_t>(
					std::lround(std::clamp(tint.alpha, 0.0F, 1.0F) * 100.0F));
				npc->tintLayers->push_back(layer);
			}

			logs::info(
				"Appearance sync built actor base: player={} revision={} base={:08X} race={:08X} female={} weight={:.1f} height={:.2f} headParts={}/{} hairColor={:08X} faceDetails={:08X} tints={}/{} droppedTints={}",
				a_playerID,
				static_cast<unsigned long long>(a_appearance.revision),
				npc->GetFormID(),
				race->GetFormID(),
				a_appearance.female,
				a_appearance.weight,
				a_appearance.height,
				headParts.size(),
				a_appearance.headParts.size(),
				hairColor ? hairColor->GetFormID() : 0,
				faceDetails ? faceDetails->GetFormID() : 0,
				npc->tintLayers->size(),
				a_appearance.tints.size(),
				droppedTints);
			return npc;
		}
	}

	void UpdateLocalAppearance(RE::PlayerCharacter& a_player, float a_delta)
	{
		static float captureElapsed = kCaptureInterval;
		static float publishElapsed = kMinimumPublishInterval;
		static std::optional<AppearanceData> published;
		static std::optional<AppearanceData> candidate;
		static std::uint64_t revision = 0;
		static bool wasConnected = false;

		const float delta = std::isfinite(a_delta) ? (std::max)(a_delta, 0.0F) : 0.0F;
		captureElapsed += delta;
		publishElapsed += delta;

		if (!Network::IsConnected()) {
			wasConnected = false;
			return;
		}
		if (!wasConnected) {
			wasConnected = true;
			// The server forgets a connection's appearance when it drops.
			if (published) {
				logs::info(
					"Appearance sync republishing after connect: revision={}",
					static_cast<unsigned long long>(published->revision));
				Network::PublishLocalAppearance(*published);
				publishElapsed = 0.0F;
			}
		}

		if (captureElapsed < kCaptureInterval) {
			return;
		}
		captureElapsed = 0.0F;

		auto captured = CaptureLocalAppearance(a_player);
		if (!captured) {
			return;
		}
		if (published && published->HasSameLook(*captured)) {
			candidate.reset();
			return;
		}
		// Require two identical captures so editor slider drags are not streamed.
		if (!candidate || !candidate->HasSameLook(*captured)) {
			candidate = std::move(captured);
			return;
		}
		if (publishElapsed < kMinimumPublishInterval) {
			return;
		}

		if (revision >= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
			logs::critical("Appearance sync cannot advance revision beyond the protocol limit");
			return;
		}
		candidate->revision = ++revision;
		if (!Multiplayer::IsValidAppearance(*candidate)) {
			logs::warn(
				"Appearance sync skipped invalid local appearance: revision={}",
				static_cast<unsigned long long>(revision));
			candidate.reset();
			return;
		}

		logs::info(
			"Appearance sync publishing local appearance: revision={} race={}|{:06X} female={} headParts={} tints={}",
			static_cast<unsigned long long>(candidate->revision),
			candidate->race.plugin,
			candidate->race.localFormID,
			candidate->female,
			candidate->headParts.size(),
			candidate->tints.size());
		Network::PublishLocalAppearance(*candidate);
		published = std::move(candidate);
		candidate.reset();
		publishElapsed = 0.0F;
	}

	void AcceptRemoteAppearance(
		RemoteAppearance& a_remote,
		const std::string& a_playerID,
		Multiplayer::PlayerAppearance&& a_appearance)
	{
		if (a_remote.latest && a_appearance.revision <= a_remote.latest->revision) {
			return;
		}

		logs::info(
			"Appearance sync accepted remote appearance: player={} revision={} race={}|{:06X} female={} headParts={} tints={}",
			a_playerID,
			static_cast<unsigned long long>(a_appearance.revision),
			a_appearance.race.plugin,
			a_appearance.race.localFormID,
			a_appearance.female,
			a_appearance.headParts.size(),
			a_appearance.tints.size());
		a_remote.latest = std::move(a_appearance);
	}

	void AdvanceRemoteAppearance(RemoteAppearance& a_remote, float a_delta)
	{
		const float delta = std::isfinite(a_delta) ? (std::max)(a_delta, 0.0F) : 0.0F;
		if (!a_remote.latest) {
			a_remote.appearanceWaitElapsed += delta;
		}
		a_remote.respawnCooldown = (std::max)(a_remote.respawnCooldown - delta, 0.0F);
	}

	RE::TESNPC* GetSpawnBase(
		RemoteAppearance& a_remote,
		const std::string& a_playerID,
		RE::TESNPC* a_localPlayerBase)
	{
		if (!a_remote.latest) {
			return a_remote.appearanceWaitElapsed >= kFirstAppearanceWaitSeconds ?
				a_localPlayerBase :
				nullptr;
		}

		const auto revision = a_remote.latest->revision;
		if (a_remote.baseRevision != revision &&
			a_remote.failedRevision != revision &&
			a_localPlayerBase) {
			if (auto* base = CreatePuppetBase(a_playerID, *a_remote.latest, *a_localPlayerBase)) {
				a_remote.base = base;
				a_remote.baseRevision = revision;
			} else {
				a_remote.failedRevision = revision;
			}
		}

		return a_remote.base && a_remote.baseRevision == revision ?
			a_remote.base :
			a_localPlayerBase;
	}

	void OnPuppetSpawned(RemoteAppearance& a_remote, RE::TESNPC* a_spawnedBase)
	{
		// A failed build also counts as spawned for that revision, so the puppet
		// is not retired every frame for an appearance that cannot be built.
		a_remote.spawnedRevision = a_remote.latest ? a_remote.latest->revision : 0;
		a_remote.spawnedBase = a_spawnedBase;
		a_remote.respawnCooldown = kMinimumRespawnInterval;
		a_remote.colorsApplied = false;
	}

	bool RequiresRespawn(const RemoteAppearance& a_remote)
	{
		return a_remote.latest &&
			a_remote.latest->revision != a_remote.spawnedRevision &&
			a_remote.respawnCooldown <= 0.0F;
	}

	void UpdateSpawnedPuppet(
		RemoteAppearance& a_remote,
		const std::string& a_playerID,
		RE::Actor& a_actor,
		bool a_actorReady)
	{
		if (a_remote.colorsApplied || !a_actorReady || !a_actor.Is3DLoaded()) {
			return;
		}
		a_remote.colorsApplied = true;
		if (!a_remote.spawnedBase || a_remote.spawnedBase != a_remote.base ||
			a_actor.GetActorBase() != a_remote.spawnedBase) {
			return;
		}

		a_actor.UpdateHairColor();
		a_actor.UpdateSkinColor();
		logs::info(
			"Appearance sync applied puppet colors: player={} actor={:08X} base={:08X} revision={} faceNode={}",
			a_playerID,
			a_actor.GetHandle().native_handle(),
			a_remote.spawnedBase->GetFormID(),
			static_cast<unsigned long long>(a_remote.spawnedRevision),
			a_actor.GetFaceNodeSkinned() != nullptr);
	}
}
