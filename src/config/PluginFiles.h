#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace PluginFiles
{
    [[nodiscard]] std::string ToUtf8(const std::filesystem::path& a_path);

    [[nodiscard]] std::optional<std::string> ReadFile(const std::filesystem::path& a_path);

    // Writes a temporary file beside the target and renames it over the target, so a
    // failed write leaves the previous contents.
    [[nodiscard]] bool ReplaceFile(const std::filesystem::path& a_path, std::string_view a_text);

    // Visits each line that is neither empty nor a ';' comment, without its line ending.
    template <class Visitor>
    void ForEachDataLine(std::string_view a_text, Visitor&& a_visitor)
    {
        while (!a_text.empty())
        {
            const auto end = a_text.find('\n');
            auto line = a_text.substr(0, end);
            a_text.remove_prefix(end == std::string_view::npos ? a_text.size() : end + 1);
            if (line.ends_with('\r'))
            {
                line.remove_suffix(1);
            }
            if (!line.empty() && line.front() != ';')
            {
                a_visitor(line);
            }
        }
    }
}
