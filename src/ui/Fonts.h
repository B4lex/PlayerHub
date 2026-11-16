#pragma once

#include <optional>

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

    // In pixels per pixel of font size, from the pen position on the baseline, y down.
    struct GlyphBounds
    {
        float left{ 0.0F };
        float top{ 0.0F };
        float right{ 0.0F };
        float bottom{ 0.0F };
    };

    // The outline's exact bounds, which a baked glyph's box widens to whole pixels.
    // Empty when the heading font is not loaded or lacks the glyph, or the glyph has no outline.
    [[nodiscard]] std::optional<GlyphBounds> FindHeadingBounds(char32_t a_code);

    // The current font size before the overlay scale, as ImGui::PushFont takes it.
    [[nodiscard]] float GetBaseSize();

    // Draws at a_pixels, a size after the overlay scale, until destroyed.
    class SizeScope
    {
    public:
        explicit SizeScope(float a_pixels);
        ~SizeScope();

        SizeScope(const SizeScope&) = delete;
        SizeScope& operator=(const SizeScope&) = delete;
    };
}
