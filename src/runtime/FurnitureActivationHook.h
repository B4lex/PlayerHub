#pragma once

namespace FurnitureActivationHook
{
    // Hooks TESFurniture::Activate to report the local player's furniture
    // activations, so remote actors blocking the furniture can yield it.
    void Install();
}
