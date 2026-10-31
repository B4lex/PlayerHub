#pragma once

#include <RE/Skyrim.h>

#include <cstdint>
#include <optional>

namespace GroundProbe
{
    struct Hit
    {
        float z{ 0.0F };
        RE::COL_LAYER layer{ RE::COL_LAYER::kUnidentified };
    };

    struct Result
    {
        std::optional<Hit> ground;
        std::uint32_t rays{ 0 };
        bool edge{ false };
    };

    [[nodiscard]] Result FindSupport(
        RE::TESObjectCELL& a_cell, const RE::NiPoint3& a_position, float a_above, float a_below);
}
