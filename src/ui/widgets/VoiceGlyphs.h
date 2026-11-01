#pragma once

#include <imgui.h>

namespace UI::VoiceGlyphs
{
    // Three bars that rise with a_level, 0 to 1, in a square of a_size whose left top is a_min.
    void DrawSpeaking(ImDrawList& a_drawList, ImVec2 a_min, float a_size, ImU32 a_color, float a_level);

    // A microphone in a square of a_size, struck through when a_off.
    void DrawMicrophone(ImDrawList& a_drawList, ImVec2 a_min, float a_size, ImU32 a_color, bool a_off);
}
