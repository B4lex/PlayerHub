#include "network/SessionStore.h"
#include "config/PluginConfig.h"
#include "config/PluginFiles.h"
#include "network/ServerClient.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace Network::SessionStore
{
    namespace
    {
        using Multiplayer::SessionToken;
        using PluginFiles::ToUtf8;

        constexpr std::string_view kFileName = "PlayerHub.session";
        constexpr std::string_view kFileHeader
            = "; Skyrim Multiplayer sessions, one per server: the session's token, then the server.\n"
              "; Keep this file to yourself. Anyone who has a line of it can connect as you.\n";
        constexpr std::string_view kHexDigits = "0123456789abcdef";

        struct Entry
        {
            std::string server;
            SessionToken token{};
        };

        std::mutex s_lock;
        // Read once; the file changes only through Save afterwards.
        bool s_loaded = false;
        std::vector<Entry> s_entries;

        std::filesystem::path GetPath()
        {
            const auto directory = Config::GetDirectory();
            return directory.empty() ? directory : directory / kFileName;
        }

        std::optional<SessionToken> ParseToken(std::string_view a_text)
        {
            SessionToken token{};
            if (a_text.size() != token.size() * 2)
            {
                return std::nullopt;
            }
            for (std::size_t i = 0; i < token.size(); ++i)
            {
                const auto high = kHexDigits.find(a_text[i * 2]);
                const auto low = kHexDigits.find(a_text[i * 2 + 1]);
                if (high == std::string_view::npos || low == std::string_view::npos)
                {
                    return std::nullopt;
                }
                token[i] = static_cast<std::uint8_t>(high << 4 | low);
            }
            return token;
        }

        void AppendToken(std::string& a_text, const SessionToken& a_token)
        {
            for (const auto byte : a_token)
            {
                a_text.push_back(kHexDigits[byte >> 4]);
                a_text.push_back(kHexDigits[byte & 0xF]);
            }
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
                logs::info("Session store has no {}; the server will issue a session", ToUtf8(path));
                return;
            }
            PluginFiles::ForEachDataLine(*text, [&path](std::string_view a_line) {
                const auto separator = a_line.find(' ');
                const auto token = ParseToken(a_line.substr(0, separator));
                if (!token || separator == std::string_view::npos || separator + 1 == a_line.size())
                {
                    logs::warn("Session store ignored a malformed line of {}", ToUtf8(path));
                    return;
                }
                s_entries.push_back(Entry{ .server = std::string{ a_line.substr(separator + 1) }, .token = *token });
            });
            logs::info("Session store loaded {} sessions from {}", s_entries.size(), ToUtf8(path));
        }

        bool WriteLocked()
        {
            const auto path = GetPath();
            if (path.empty())
            {
                return false;
            }
            std::string text{ kFileHeader };
            for (const auto& entry : s_entries)
            {
                AppendToken(text, entry.token);
                text.push_back(' ');
                text.append(entry.server);
                text.push_back('\n');
            }
            return PluginFiles::ReplaceFile(path, text);
        }
    }

    std::optional<SessionToken> Find(std::string_view a_server)
    {
        const std::scoped_lock lock{ s_lock };
        LoadLocked();
        const auto entry = std::ranges::find(s_entries, a_server, &Entry::server);
        if (entry == s_entries.end())
        {
            return std::nullopt;
        }
        return entry->token;
    }

    bool Save(std::string_view a_server, const SessionToken& a_token)
    {
        const std::scoped_lock lock{ s_lock };
        LoadLocked();
        const auto entry = std::ranges::find(s_entries, a_server, &Entry::server);
        if (entry == s_entries.end())
        {
            s_entries.push_back(Entry{ .server = std::string{ a_server }, .token = a_token });
        }
        else
        {
            entry->token = a_token;
        }
        if (!WriteLocked())
        {
            logs::error(
                "Session store could not save the session with {}; it lasts until the game exits, and "
                "its display name stays taken on the server until the session expires",
                DescribeServer(a_server));
            return false;
        }
        logs::debug("Session store saved the session with {}", DescribeServer(a_server));
        return true;
    }
}
