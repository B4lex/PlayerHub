#pragma once

struct ImFont;
struct ImFontAtlas;

namespace UI::Fonts
{
    void Load(ImFontAtlas& a_atlas, float a_sizePixels);

    [[nodiscard]] ImFont* GetText();
    [[nodiscard]] ImFont* GetHeading();
}
