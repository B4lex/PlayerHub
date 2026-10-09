#pragma once

namespace UI::Backdrop
{
    // Covers the display behind every overlay window with the game's finished frame, blurred and
    // darkened, or only darkened while the blur is unavailable. Once per frame at most.
    void Draw();

    // Before ImGui::NewFrame: frees the textures when no window drew the backdrop last frame.
    void Update();
}
