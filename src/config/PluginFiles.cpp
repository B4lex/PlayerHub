#include "config/PluginFiles.h"

#include <fstream>
#include <iterator>
#include <system_error>

namespace PluginFiles
{
    namespace
    {
        constexpr std::string_view kTemporaryExtension = ".tmp";
    }

    std::string ToUtf8(const std::filesystem::path& a_path)
    {
        const auto text = a_path.u8string();
        return { text.begin(), text.end() };
    }

    std::optional<std::string> ReadFile(const std::filesystem::path& a_path)
    {
        std::ifstream input{ a_path, std::ios::binary };
        if (!input)
        {
            return std::nullopt;
        }
        return std::string{ std::istreambuf_iterator<char>{ input }, std::istreambuf_iterator<char>{} };
    }

    bool ReplaceFile(const std::filesystem::path& a_path, std::string_view a_text)
    {
        auto temporaryPath = a_path;
        temporaryPath += kTemporaryExtension;
        {
            std::ofstream output{ temporaryPath, std::ios::binary | std::ios::trunc };
            output.write(a_text.data(), static_cast<std::streamsize>(a_text.size()));
            output.close();
            if (!output)
            {
                return false;
            }
        }

        std::error_code error;
        std::filesystem::rename(temporaryPath, a_path, error);
        if (error)
        {
            logs::warn("Could not replace {}: {}", ToUtf8(a_path), error.message());
            std::filesystem::remove(temporaryPath, error);
            return false;
        }
        return true;
    }
}
