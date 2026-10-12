#pragma once

#include "ui/InputCapture.h"
#include "ui/InputLanguage.h"

#include <cstddef>

namespace UI::Overlay
{
    inline constexpr std::size_t kTrampolineSize = 14 + InputCapture::kTrampolineSize + InputLanguage::kTrampolineSize;

    void Install();
}
