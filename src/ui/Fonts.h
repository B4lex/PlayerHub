#pragma once

struct ImFontAtlas;

namespace UI::Fonts
{
    // Adds the overlay font: a system UI font, merged with system fallbacks
    // that cover other scripts, such as Cyrillic, Arabic, Indic, and CJK. The
    // files are memory-mapped for the process lifetime, so only glyphs that
    // are drawn are read. Uses Dear ImGui's Latin-only default font when no
    // system font loads.
    void Load(ImFontAtlas& a_atlas, float a_sizePixels);
}
