#include "network/PlayerFilterStore.h"
#include "config/PluginConfig.h"
#include "config/PluginFiles.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace Network::PlayerFilterStore
{
    namespace
    {
        using PluginFiles::ToUtf8;

        constexpr std::string_view kFileName = "PlayerHub.players";
        constexpr std::string_view kFileHeader
            = "; Skyrim Multiplayer: your friends (f) and the players you muted (m) or vanished (v), per server.\n"
              "; Each line: any of f, m, and v, the player's session, the server, and its name when last seen,\n"
              "; separated by tabs.\n";
        constexpr char kSeparator = '\t';

        struct StoredEntry
        {
            std::string server;
            Entry entry;
        };

        std::mutex s_lock;
        // Read once; the file changes only through Save afterwards.
        bool s_loaded = false;
        std::vector<StoredEntry> s_entries;

        bool IsKept(const Entry& a_entry)
        {
            return a_entry.friended || a_entry.muted || a_entry.vanished;
        }

        std::filesystem::path GetPath()
        {
            const auto directory = Config::GetDirectory();
            return directory.empty() ? directory : directory / kFileName;
        }

        std::optional<StoredEntry> ParseLine(std::string_view a_line)
        {
            std::array<std::string_view, 4> fields;
            for (std::size_t index = 0; index < fields.size(); ++index)
            {
                const auto end = index + 1 == fields.size() ? a_line.size() : a_line.find(kSeparator);
                if (end == std::string_view::npos)
                {
                    return std::nullopt;
                }
                fields[index] = a_line.substr(0, end);
                a_line.remove_prefix((std::min)(end + 1, a_line.size()));
            }
            const auto& [flags, session, server, name] = fields;

            StoredEntry result{ .server = std::string{ server }, .entry = { .name = std::string{ name } } };
            const auto parsed = std::from_chars(session.data(), session.data() + session.size(), result.entry.session);
            if (parsed.ec != std::errc{} || parsed.ptr != session.data() + session.size() || result.entry.session == 0
                || server.empty() || name.empty())
            {
                return std::nullopt;
            }
            result.entry.friended = flags.find('f') != std::string_view::npos;
            result.entry.muted = flags.find('m') != std::string_view::npos;
            result.entry.vanished = flags.find('v') != std::string_view::npos;
            if (flags.find_first_not_of("fmv") != std::string_view::npos || !IsKept(result.entry))
            {
                return std::nullopt;
            }
            return result;
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
                logs::info("Player filters have no {}; nobody is a friend, muted, or vanished", ToUtf8(path));
                return;
            }
            PluginFiles::ForEachDataLine(*text, [&path](std::string_view a_line) {
                if (auto entry = ParseLine(a_line))
                {
                    s_entries.push_back(std::move(*entry));
                }
                else
                {
                    logs::warn("Player filters ignored a malformed line of {}", ToUtf8(path));
                }
            });
            logs::info("Player filters loaded {} players from {}", s_entries.size(), ToUtf8(path));
        }

        bool WriteLocked()
        {
            const auto path = GetPath();
            if (path.empty())
            {
                return false;
            }
            std::string text{ kFileHeader };
            for (const auto& [server, entry] : s_entries)
            {
                if (entry.friended)
                {
                    text.push_back('f');
                }
                if (entry.muted)
                {
                    text.push_back('m');
                }
                if (entry.vanished)
                {
                    text.push_back('v');
                }
                text.push_back(kSeparator);
                text.append(std::to_string(entry.session));
                text.push_back(kSeparator);
                text.append(server);
                text.push_back(kSeparator);
                text.append(entry.name);
                text.push_back('\n');
            }
            return PluginFiles::ReplaceFile(path, text);
        }
    }

    void Copy(std::string_view a_server, std::vector<Entry>& a_entries)
    {
        const std::scoped_lock lock{ s_lock };
        LoadLocked();
        for (const auto& [server, entry] : s_entries)
        {
            if (server == a_server)
            {
                a_entries.push_back(entry);
            }
        }
    }

    void CopyVanished(std::string_view a_server, std::vector<Multiplayer::SessionID>& a_sessions)
    {
        const std::scoped_lock lock{ s_lock };
        LoadLocked();
        for (const auto& [server, entry] : s_entries)
        {
            if (server == a_server && entry.vanished)
            {
                a_sessions.push_back(entry.session);
            }
        }
    }

    bool Save(std::string_view a_server, const Entry& a_entry)
    {
        const std::scoped_lock lock{ s_lock };
        LoadLocked();
        const auto stored = std::ranges::find_if(s_entries, [&](const StoredEntry& a_stored) {
            return a_stored.entry.session == a_entry.session && a_stored.server == a_server;
        });
        if (!IsKept(a_entry))
        {
            if (stored != s_entries.end())
            {
                s_entries.erase(stored);
            }
        }
        else if (stored == s_entries.end())
        {
            s_entries.push_back(StoredEntry{ .server = std::string{ a_server }, .entry = a_entry });
        }
        else
        {
            stored->entry = a_entry;
        }
        if (!WriteLocked())
        {
            logs::error("Player filters could not save {}; the change lasts until the game exits", kFileName);
            return false;
        }
        return true;
    }
}
