#include "forms/FormLookup.h"

namespace FormLookup
{
    namespace
    {
        constexpr std::uint32_t kLightPluginLocalFormIDMask = 0x00000FFF;
        constexpr std::uint32_t kPluginLocalFormIDMask = 0x00FFFFFF;
        constexpr std::uint8_t kLightPluginCompileIndex = 0xFE;
    }

    const RE::TESFile* GetDefiningFile(const RE::TESForm* a_form)
    {
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!a_form || a_form->IsDynamicForm() || !dataHandler)
        {
            return nullptr;
        }

        // TESForm::GetFile(0) can be an overriding file: Skyrim.esm's Arcane
        // Enchanter reference 000DDFB3 lists only Update.esm. ResolveFormID
        // rebuilds the ID from the plugin's compile index, so name that one.
        const auto formID = a_form->GetFormID();
        const auto compileIndex = static_cast<std::uint8_t>(formID >> 24);
        if (compileIndex == kLightPluginCompileIndex)
        {
            return dataHandler->LookupLoadedLightModByIndex(static_cast<std::uint16_t>((formID >> 12) & 0xFFF));
        }
        return dataHandler->LookupLoadedModByIndex(compileIndex);
    }

    std::optional<Multiplayer::FormReference> MakeFormReference(const RE::TESForm* a_form)
    {
        const auto* file = GetDefiningFile(a_form);
        if (!file)
        {
            return std::nullopt;
        }

        const auto fileName = file->GetFilename();
        if (fileName.empty() || fileName.size() > Multiplayer::FormReference::kMaximumPluginNameLength)
        {
            return std::nullopt;
        }

        return Multiplayer::FormReference{ .plugin = std::string{ fileName },
            .localFormID
            = a_form->GetFormID() & (file->IsLight() ? kLightPluginLocalFormIDMask : kPluginLocalFormIDMask) };
    }

    RE::FormID ResolveFormID(const Multiplayer::FormReference& a_reference)
    {
        if (a_reference.IsEmpty())
        {
            return 0;
        }

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        const auto* file = dataHandler ? dataHandler->LookupModByName(a_reference.plugin) : nullptr;
        // A light plugin's local IDs are 12 bits; a wider one would carry into
        // the compile index and name another plugin's form.
        if (!file || (file->IsLight() && a_reference.localFormID > kLightPluginLocalFormIDMask))
        {
            return 0;
        }
        return dataHandler->LookupFormID(a_reference.localFormID, a_reference.plugin);
    }

    RE::TESForm* ResolveForm(const Multiplayer::FormReference& a_reference)
    {
        const auto formID = ResolveFormID(a_reference);
        return formID ? RE::TESForm::LookupByID(formID) : nullptr;
    }
}
