#include "FormLookup.h"

namespace FormLookup
{
	namespace
	{
		constexpr std::uint32_t kLightPluginLocalFormIDMask = 0x00000FFF;
		constexpr std::uint32_t kPluginLocalFormIDMask = 0x00FFFFFF;
	}

	std::optional<Multiplayer::FormReference> MakeFormReference(const RE::TESForm* a_form)
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
			fileName.size() > Multiplayer::FormReference::kMaximumPluginNameLength) {
			return std::nullopt;
		}

		return Multiplayer::FormReference{
			.plugin = std::string{ fileName },
			.localFormID = a_form->GetFormID() &
				(file->IsLight() ? kLightPluginLocalFormIDMask : kPluginLocalFormIDMask)
		};
	}

	RE::TESForm* ResolveForm(const Multiplayer::FormReference& a_reference)
	{
		if (a_reference.IsEmpty()) {
			return nullptr;
		}

		auto* dataHandler = RE::TESDataHandler::GetSingleton();
		return dataHandler ?
			dataHandler->LookupForm(a_reference.localFormID, a_reference.plugin) :
			nullptr;
	}
}
