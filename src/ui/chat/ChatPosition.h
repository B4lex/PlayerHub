#pragma once

#include <imgui.h>

namespace UI
{
    class ChatPosition
    {
    public:
        // The lower left corner of the open chat's frame.
        [[nodiscard]] ImVec2 Get() const;

        [[nodiscard]] bool IsHeld() const { return held; }

        void Follow(const ImVec2& a_frameSize);

        void Grab(const ImVec2& a_corner);

        void Release();

    private:
        bool held{ false };
        bool moved{ false };
        ImVec2 origin;
        ImVec2 corner;
        ImVec2 frameSize;
    };
}
