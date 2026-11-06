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
