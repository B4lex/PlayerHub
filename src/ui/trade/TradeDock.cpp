#include "ui/trade/TradeDock.h"

namespace UI::TradeDock
{
    namespace
    {
        // Both on the overlay thread.
        Column s_column;
        int s_frame{ -1 };
    }

    void Set(const Column& a_column)
    {
        s_column = a_column;
        s_frame = ImGui::GetFrameCount();
    }

    std::optional<Column> Get()
    {
        return s_frame == ImGui::GetFrameCount() ? std::optional{ s_column } : std::nullopt;
    }
}
