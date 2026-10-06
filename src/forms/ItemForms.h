#pragma once

#include "forms/ItemFilter.h"

#include <RE/Skyrim.h>

#include <optional>

namespace Forms
{
    [[nodiscard]] std::optional<ItemTraits> GetItemTraits(RE::TESForm& a_form);

    void BuildItemCatalog();
}
