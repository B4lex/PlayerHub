#pragma once

#include <cstddef>
#include <cstdint>

namespace UI::InputLanguage
{
    // The pointer slots write_call<6> allocates for its two call sites.
    inline constexpr std::size_t kTrampolineSize = 2 * sizeof(std::uintptr_t);

    void Install();
}
