#pragma once

struct ImFont;
struct ImFontAtlas;

namespace UI::Fonts
{
    void Load(ImFontAtlas& a_atlas, float a_sizePixels);

    // Loads the fonts again when the language has changed to one that puts another
    // Han font first. Call between frames.
    void Update(ImFontAtlas& a_atlas, float a_sizePixels);

    [[nodiscard]] ImFont* GetText();
    [[nodiscard]] ImFont* GetHeading();
    [[nodiscard]] ImFont* GetLogo();
}
