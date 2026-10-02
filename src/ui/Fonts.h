#pragma once

struct ImFont;
struct ImFontAtlas;

namespace UI::Fonts
{
    // Adds the overlay font: a system UI font, merged with system fallbacks
    // that cover other scripts, such as Cyrillic, Arabic, Indic, and CJK. The
    // files are memory-mapped for the process lifetime, so only glyphs that
    // are drawn are read. Uses Dear ImGui's Latin-only default font when no
    // system font loads. Then adds the themed faces from the files shipped
    // beside the plugin, each over the same system fallbacks.
    void Load(ImFontAtlas& a_atlas, float a_sizePixels);

    // The faces of the plugin's themed windows and chat, for ImGui::PushFont. Null
    // before Load and when the shipped file is missing or unreadable;
    // PushFont then keeps the current font. Overlay thread.
    [[nodiscard]] ImFont* GetText();
    [[nodiscard]] ImFont* GetHeading();
}
