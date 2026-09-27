#pragma once

#include <imgui.h>

#include <cstdint>

// Shows an item's 3D model in the overlay the way the inventory does: the game
// renders it through its Inventory3DManager, and that part of the frame is copied
// into a texture before the overlay draws. One item at a time. Overlay thread.
namespace UI::ItemPreview
{
    // Draws the model of the form with a_formID, fitted into a_size, at the
    // cursor. Keeps the inventory renderer running while some window draws a
    // preview each frame. The model appears once the game has loaded it, at the
    // earliest on the next frame. A form ID of zero draws an empty preview.
    void Draw(std::uint32_t a_formID, ImVec2 a_size);

    // Before ImGui::NewFrame: renders the item the previous frame drew, or
    // releases the inventory renderer when none did.
    void Update();
}
