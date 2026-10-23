#pragma once

#include <RE/Skyrim.h>

#include <optional>

namespace GroundProbe
{
    struct Hit
    {
        float z{ 0.0F };
        RE::COL_LAYER layer{ RE::COL_LAYER::kUnidentified };
    };

    [[nodiscard]] std::optional<Hit> FindGround(
        RE::TESObjectCELL& a_cell, const RE::NiPoint3& a_position, float a_above, float a_below);
}
