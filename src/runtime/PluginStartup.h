#pragma once

#include <cstddef>

namespace PluginStartup
{
    // The SKSE trampoline must hold this many bytes before Install writes any hook.
    [[nodiscard]] std::size_t TrampolineSize();
    void Install();
}
