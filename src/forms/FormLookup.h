#pragma once

#include "protocol/FormReference.h"

#include <RE/Skyrim.h>

#include <optional>

namespace FormLookup
{
    // Returns nullopt for dynamic (FF) forms and forms without a defining file,
    // which cannot be referenced across game sessions.
    [[nodiscard]] std::optional<Multiplayer::FormReference> MakeFormReference(const RE::TESForm* a_form);

    // The local FormID of a reference, whether or not the form is loaded. Zero
    // when the plugin is not loaded here or the ID lies outside its range.
    [[nodiscard]] RE::FormID ResolveFormID(const Multiplayer::FormReference& a_reference);

    // Resolves by plugin name, independent of the local load order.
    [[nodiscard]] RE::TESForm* ResolveForm(const Multiplayer::FormReference& a_reference);

    template <class T>
    [[nodiscard]] T* ResolveForm(const Multiplayer::FormReference& a_reference)
    {
        auto* form = ResolveForm(a_reference);
        return form ? form->As<T>() : nullptr;
    }
}
