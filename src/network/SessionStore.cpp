#include "network/SessionStore.h"
#include "config/PluginConfig.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace Network::SessionStore
{
    namespace
    {
        using Multiplayer::SessionToken;

        constexpr std::string_view kFileName = "skyrim-mp.session";
        constexpr std::string_view kTemporaryExtension = ".tmp";
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

        std::string ToUtf8(const std::filesystem::path& a_path)
        {
            const auto text = a_path.u8string();
            return { text.begin(), text.end() };
        }

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

        // Requires s_lock.
        void LoadLocked()
        {
            const auto path = GetPath();
            if (s_loaded || path.empty())
            {
                return;
            }
            s_loaded = true;

            std::ifstream input{ path, std::ios::binary };
            if (!input)
            {
                logs::info("Session store has no {}; the server will issue a session", ToUtf8(path));
                return;
            }
            std::string line;
            while (std::getline(input, line))
            {
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                if (line.empty() || line.front() == ';')
                {
                    continue;
                }
                const auto separator = line.find(' ');
                const auto token = ParseToken(std::string_view{ line }.substr(0, separator));
                if (!token || separator == std::string::npos || separator + 1 == line.size())
                {
                    logs::warn("Session store ignored a malformed line of {}", ToUtf8(path));
                    continue;
                }
                s_entries.push_back(Entry{ .server = line.substr(separator + 1), .token = *token });
            }
            logs::info("Session store loaded {} sessions from {}", s_entries.size(), ToUtf8(path));
        }

        // Through a temporary file, so a failed write never leaves a truncated
        // file. Requires s_lock.
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

            auto temporaryPath = path;
            temporaryPath += kTemporaryExtension;
            {
                std::ofstream output{ temporaryPath, std::ios::binary | std::ios::trunc };
                output.write(text.data(), static_cast<std::streamsize>(text.size()));
                output.close();
                if (!output)
                {
                    return false;
                }
            }
            std::error_code error;
            std::filesystem::rename(temporaryPath, path, error);
            if (error)
            {
                logs::warn("Session store could not replace {}: {}", ToUtf8(path), error.message());
                std::filesystem::remove(temporaryPath, error);
                return false;
            }
            return true;
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
            // The session holds the nickname on the server, so a later game
            // without its token finds the nickname taken.
            logs::error(
                "Session store could not save the session with {}; it lasts until the game exits, and "
                "its nickname stays taken on the server until the session expires",
                a_server);
            return false;
        }
        logs::info("Session store saved the session with {}", a_server);
        return true;
    }
}
