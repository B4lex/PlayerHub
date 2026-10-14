#pragma once

#include "protocol/FormReference.h"

#include <RE/Skyrim.h>

#include <optional>
#include <string_view>

namespace FormLookup
{
    // The plugin the FormID's compile index names; null for dynamic (FF) forms.
    [[nodiscard]] const RE::TESFile* GetDefiningFile(const RE::TESForm* a_form);

    // Dynamic (FF) forms and forms without a defining file cannot be referenced
    // across game sessions.
    [[nodiscard]] std::optional<Multiplayer::FormReference> MakeFormReference(const RE::TESForm* a_form);

    [[nodiscard]] bool IsPluginLoaded(std::string_view a_plugin);

    [[nodiscard]] RE::FormID ResolveFormID(const Multiplayer::FormReference& a_reference);

    [[nodiscard]] RE::TESForm* ResolveForm(const Multiplayer::FormReference& a_reference);

    template <class T>
    [[nodiscard]] T* ResolveForm(const Multiplayer::FormReference& a_reference)
    {
        auto* form = ResolveForm(a_reference);
        return form ? form->As<T>() : nullptr;
    }
}
