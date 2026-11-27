#pragma once

#include "sync/unsynced/UnsyncedCells.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace UI
{
    // The list of unsynced interiors, a framed window over the settings window.
    class UnsyncedLocations
    {
    public:
        void Open();
        void Close() { open = false; }
        [[nodiscard]] bool IsOpen() const { return open; }

        // After the settings window, inside its theme.
        void Draw();

    private:
        struct Row
        {
            UnsyncedCells::Entry entry;
            std::string folded;
            std::string reference;
        };

        void Refresh();
        void Match();
        void DrawContents();
        void DrawLocalCell();
        void DrawRows();

        bool open{ false };
        bool focusRequested{ false };
        std::optional<std::uint64_t> revision;
        std::vector<Row> rows;
        std::vector<std::uint32_t> matches;
        std::array<char, 64> search{};
        std::string foldedSearch;
    };
}
