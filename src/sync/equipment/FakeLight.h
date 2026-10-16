#pragma once

#include <RE/Skyrim.h>

namespace FakeLight
{
    // The light itself when it casts no shadow, otherwise its copy as a fake light.
    [[nodiscard]] RE::TESObjectLIGH& Get(RE::TESObjectLIGH& a_light);
}
