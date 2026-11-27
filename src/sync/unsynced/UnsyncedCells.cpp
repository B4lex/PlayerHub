#include "sync/unsynced/UnsyncedCells.h"
#include "config/PluginConfig.h"
#include "config/PluginFiles.h"
#include "config/SettingsSchema.h"
#include "forms/FormLookup.h"
#include "runtime/GameThread.h"
#include "sync/notices/Notices.h"
#include "text/AsciiCase.h"
#include "text/Text.h"

#include <RE/Skyrim.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string_view>

namespace UnsyncedCells
{
    namespace
    {
        using PluginFiles::ToUtf8;

        constexpr std::string_view kFileName = "PlayerHub.locations";
        constexpr std::string_view kFileHeader
            = "; PlayerHub: interiors where you see and hear no other players, on every server.\n"
              "; Each line: the cell as Plugin.esp|0xFormID and its name when added, separated by a tab.\n";
        constexpr char kSeparator = '\t';

        std::mutex s_lock;
        // Read once; the file changes only through this module afterwards.
        bool s_loaded = false;
        std::vector<Entry> s_entries;
        std::optional<LocalCell> s_localCell;
        std::atomic_uint64_t s_revision{ 0 };

        // Game thread only.
        RE::FormID s_observedCellID = 0;
        std::uint64_t s_observedRevision = (std::numeric_limits<std::uint64_t>::max)();
        std::optional<Entry> s_observedCell;
        bool s_hidden = false;

        bool IsSameCell(const Multiplayer::FormReference& a_left, const Multiplayer::FormReference& a_right)
        {
            return a_left.localFormID == a_right.localFormID && AsciiCase::Equals(a_left.plugin, a_right.plugin);
        }

        std::vector<Entry>::iterator FindLocked(const Multiplayer::FormReference& a_cell)
        {
            return std::ranges::find_if(
                s_entries, [&a_cell](const Entry& a_entry) { return IsSameCell(a_entry.cell, a_cell); });
        }

        std::filesystem::path GetPath()
        {
            const auto directory = Config::GetDirectory();
            return directory.empty() ? directory : directory / kFileName;
        }

        std::optional<Entry> ParseLine(std::string_view a_line)
        {
            const auto separator = a_line.find(kSeparator);
            if (separator == std::string_view::npos || separator + 1 == a_line.size())
            {
                return std::nullopt;
            }
            auto cell = Config::ParseFormReference(a_line.substr(0, separator));
            if (!cell)
            {
                return std::nullopt;
            }
            return Entry{ .cell = std::move(*cell), .name = std::string{ a_line.substr(separator + 1) } };
        }

        void LoadLocked()
        {
            const auto path = GetPath();
            if (s_loaded || path.empty())
            {
                return;
            }
            s_loaded = true;

            const auto text = PluginFiles::ReadFile(path);
            if (!text)
            {
                logs::info("Unsynced cells have no {}; every location is synced", ToUtf8(path));
                return;
            }
            PluginFiles::ForEachDataLine(*text, [&path](std::string_view a_line) {
                auto entry = ParseLine(a_line);
                if (!entry)
                {
                    logs::warn("Unsynced cells ignored a malformed line of {}", ToUtf8(path));
                }
                else if (FindLocked(entry->cell) == s_entries.end())
                {
                    s_entries.push_back(std::move(*entry));
                }
            });
            logs::info("Unsynced cells loaded {} locations from {}", s_entries.size(), ToUtf8(path));
        }

        bool WriteLocked()
        {
            const auto path = GetPath();
            if (path.empty())
            {
                return false;
            }
            std::string text{ kFileHeader };
            for (const auto& [cell, name] : s_entries)
            {
                text.append(Config::FormatFormReference(cell));
                text.push_back(kSeparator);
                text.append(name);
                text.push_back('\n');
            }
            if (!PluginFiles::ReplaceFile(path, text))
            {
                logs::error("Unsynced cells could not save {}; the change lasts until the game exits", kFileName);
                return false;
            }
            return true;
        }

        void ChangedLocked()
        {
            s_revision.fetch_add(1, std::memory_order_acq_rel);
            if (s_localCell)
            {
                s_localCell->unsynced = FindLocked(s_localCell->entry.cell) != s_entries.end();
            }
        }

        // The file keeps one line per location.
        std::string GetCellName(const RE::TESObjectCELL& a_cell, const Multiplayer::FormReference& a_reference)
        {
            const char* name = a_cell.GetFullName();
            if (!name || *name == '\0')
            {
                name = a_cell.GetFormEditorID();
            }
            std::string result = name && *name != '\0' ? std::string{ name } : Config::FormatFormReference(a_reference);
            std::ranges::replace_if(
                result,
                [](char a_character) { return a_character == '\t' || a_character == '\r' || a_character == '\n'; },
                ' ');
            return result;
        }

        std::optional<Entry> MakeEntry(const RE::TESObjectCELL& a_cell)
        {
            auto reference = FormLookup::MakeFormReference(&a_cell);
            if (!reference)
            {
                logs::debug("Unsynced cells cannot name interior {:08X}, which has no plugin", a_cell.GetFormID());
                return std::nullopt;
            }
            auto name = GetCellName(a_cell, *reference);
            return Entry{ .cell = std::move(*reference), .name = std::move(name) };
        }
    }

    std::uint64_t GetRevision()
    {
        return s_revision.load(std::memory_order_acquire);
    }

    void Copy(std::vector<Entry>& a_entries)
    {
        const std::scoped_lock lock{ s_lock };
        LoadLocked();
        a_entries = s_entries;
    }

    std::optional<LocalCell> GetLocalCell()
    {
        const std::scoped_lock lock{ s_lock };
        return s_localCell;
    }

    void AddLocalCell()
    {
        bool saved = true;
        {
            const std::scoped_lock lock{ s_lock };
            LoadLocked();
            if (!s_localCell || FindLocked(s_localCell->entry.cell) != s_entries.end())
            {
                return;
            }
            s_entries.push_back(s_localCell->entry);
            ChangedLocked();
            logs::info("Unsynced cells added {} ({})", s_localCell->entry.name,
                Config::FormatFormReference(s_localCell->entry.cell));
            saved = WriteLocked();
        }
        if (!saved)
        {
            Notices::Post(std::string{ Text::Get(Text::Id::kUnsyncedSaveFailed) });
        }
    }

    void Remove(const Multiplayer::FormReference& a_cell)
    {
        bool saved = true;
        {
            const std::scoped_lock lock{ s_lock };
            LoadLocked();
            const auto entry = FindLocked(a_cell);
            if (entry == s_entries.end())
            {
                return;
            }
            logs::info("Unsynced cells removed {} ({})", entry->name, Config::FormatFormReference(entry->cell));
            s_entries.erase(entry);
            ChangedLocked();
            saved = WriteLocked();
        }
        if (!saved)
        {
            Notices::Post(std::string{ Text::Get(Text::Id::kUnsyncedSaveFailed) });
        }
    }

    bool UpdateLocal(const RE::TESObjectCELL* a_cell)
    {
        if (!GameThread::Require("UnsyncedCells::UpdateLocal"))
        {
            return s_hidden;
        }
        const RE::FormID cellID = a_cell && a_cell->IsInteriorCell() ? a_cell->GetFormID() : 0;
        const auto revision = GetRevision();
        if (cellID == s_observedCellID && revision == s_observedRevision)
        {
            return s_hidden;
        }
        if (cellID != s_observedCellID)
        {
            s_observedCellID = cellID;
            s_observedCell = cellID != 0 ? MakeEntry(*a_cell) : std::nullopt;
        }
        s_observedRevision = revision;

        bool hidden = false;
        {
            const std::scoped_lock lock{ s_lock };
            LoadLocked();
            hidden = s_observedCell && FindLocked(s_observedCell->cell) != s_entries.end();
            s_localCell = s_observedCell ? std::optional{ LocalCell{ .entry = *s_observedCell, .unsynced = hidden } }
                                         : std::nullopt;
        }
        if (hidden != s_hidden)
        {
            if (hidden)
            {
                logs::info("Unsynced cells hide remote players in {} ({})", s_observedCell->name,
                    Config::FormatFormReference(s_observedCell->cell));
            }
            else
            {
                logs::info("Unsynced cells show remote players again");
            }
            s_hidden = hidden;
        }
        return hidden;
    }
}
