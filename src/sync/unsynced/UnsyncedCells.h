#pragma once

#include "protocol/FormReference.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace RE
{
    class TESObjectCELL;
}

namespace UnsyncedCells
{
    struct Entry
    {
        Multiplayer::FormReference cell;
        std::string name;
    };

    struct LocalCell
    {
        Entry entry;
        bool unsynced{ false };
    };

    // Any thread.
    [[nodiscard]] std::uint64_t GetRevision();
    void Copy(std::vector<Entry>& a_entries);
    [[nodiscard]] std::optional<LocalCell> GetLocalCell();
    void AddLocalCell();
    void Remove(const Multiplayer::FormReference& a_cell);

    // Game thread, once per pass; null without a cell. Returns whether remote players are hidden.
    [[nodiscard]] bool UpdateLocal(const RE::TESObjectCELL* a_cell);
}
