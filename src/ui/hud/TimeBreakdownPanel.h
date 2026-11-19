#pragma once

#ifdef PLAYERHUB_TIME_BREAKDOWN
#    include "ui/Window.h"

#    include <memory>

namespace UI
{
    [[nodiscard]] std::unique_ptr<Window> CreateTimeBreakdownPanel();
}
#endif
