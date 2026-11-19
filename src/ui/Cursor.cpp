#include "ui/Cursor.h"
#include "ui/Fonts.h"
#include "ui/InputCapture.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace UI::Cursor
{
    namespace
    {
        constexpr float kHeightFonts = 2.0F;
        constexpr float kTooltipGapFonts = 0.25F;
        // Dear ImGui keeps a tooltip out of the box from the mouse to 24 px times
        // MouseCursorScale right and down, beside it when it fits and else below it
        // (FindBestWindowPosForTooltip in 1.92.9), for a cursor of its own size.
        constexpr float kImGuiTooltipAvoid = 24.0F;
        // The font size at which Dear ImGui's own cursor, the fallback, draws at scale 1.
        constexpr float kImGuiCursorFontPixels = 16.0F;
        constexpr ImU32 kOutline = IM_COL32(0, 0, 0, 204);
        constexpr ImU32 kBand = IM_COL32(187, 189, 191, 153);
        constexpr ImU32 kCentre = IM_COL32(0, 0, 0, 255);

        // One PlayerHub.ttf glyph per fill, each placing the cursor's hot spot at the
        // text position.
        struct Glyphs
        {
            const char* outline;
            const char* band;
            const char* centre;
        };

        constexpr Glyphs kPointer{ "\xEE\x80\x85", "\xEE\x80\x86", "\xEE\x80\x87" };
        constexpr Glyphs kMove{ "\xEE\x80\x90", "\xEE\x80\x91", "\xEE\x80\x92" };
        constexpr Glyphs kResizeEw{ "\xEE\x80\x93", "\xEE\x80\x94", "\xEE\x80\x95" };
        constexpr Glyphs kResizeNs{ "\xEE\x80\x96", "\xEE\x80\x97", "\xEE\x80\x98" };
        constexpr Glyphs kResizeNesw{ "\xEE\x80\x99", "\xEE\x80\x9A", "\xEE\x80\x9B" };
        constexpr Glyphs kResizeNwse{ "\xEE\x80\x9C", "\xEE\x80\x9D", "\xEE\x80\x9E" };
        constexpr Glyphs kText{ "\xEE\x80\x9F", "\xEE\x80\xA0", nullptr };

        const Glyphs& GetGlyphs(ImGuiMouseCursor a_cursor)
        {
            switch (a_cursor)
            {
            case ImGuiMouseCursor_TextInput:
                return kText;
            case ImGuiMouseCursor_ResizeAll:
                return kMove;
            case ImGuiMouseCursor_ResizeEW:
                return kResizeEw;
            case ImGuiMouseCursor_ResizeNS:
                return kResizeNs;
            case ImGuiMouseCursor_ResizeNESW:
                return kResizeNesw;
            case ImGuiMouseCursor_ResizeNWSE:
                return kResizeNwse;
            default:
                return kPointer;
            }
        }
    }

    void PlaceTooltips(ImGuiStyle& a_style)
    {
        const auto fontSize = a_style.FontSizeBase * a_style.FontScaleMain * a_style.FontScaleDpi;
        if (!Fonts::GetLogo())
        {
            a_style.MouseCursorScale = fontSize / kImGuiCursorFontPixels;
            return;
        }
        // The normal cursor is a little narrower than it is tall.
        a_style.MouseCursorScale = (kHeightFonts + kTooltipGapFonts) * fontSize / kImGuiTooltipAvoid;
    }

    void Draw()
    {
        if (!InputCapture::ShowsCursor())
        {
            return;
        }
        const auto cursor = ImGui::GetMouseCursor();
        if (cursor == ImGuiMouseCursor_None || !ImGui::IsMousePosValid())
        {
            return;
        }

        const auto& mouse = ImGui::GetIO().MousePos;
        auto* const font = Fonts::GetLogo();
        if (!font)
        {
            ImGui::RenderMouseCursor(mouse, ImGui::GetStyle().MouseCursorScale, cursor, IM_COL32_WHITE,
                IM_COL32_BLACK, IM_COL32(0, 0, 0, 48));
            return;
        }

        const auto& glyphs = GetGlyphs(cursor);
        const auto height = kHeightFonts * ImGui::GetFontSize();
        auto& list = *ImGui::GetForegroundDrawList();
        list.AddText(font, height, mouse, kOutline, glyphs.outline);
        list.AddText(font, height, mouse, kBand, glyphs.band);
        if (glyphs.centre)
        {
            list.AddText(font, height, mouse, kCentre, glyphs.centre);
        }
    }
}
