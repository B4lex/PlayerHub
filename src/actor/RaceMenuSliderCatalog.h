#pragma once

#include "actor/SKEEInterfaces.h"

#include <RE/Skyrim.h>

#include <string>
#include <vector>

namespace RaceMenuSliderCatalog
{
    struct Slider
    {
        std::string name;
        SKEEInterfaces::SKEEString key;
    };

    // The value sliders (SKEE types Slider and Preset) that RaceMenu's FaceGenMorphs
    // folders give a race and sex, sorted by name. Read on the first call.
    [[nodiscard]] const std::vector<Slider>& Get(const RE::TESRace& a_race, bool a_female);

    // Every slider of every race and sex once, compared without case.
    [[nodiscard]] const std::vector<Slider>& GetAll();
}
