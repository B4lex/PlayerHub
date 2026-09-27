#include "ui/Fonts.h"

#include <REX/W32/KERNEL32.h>
#include <REX/W32/OLE32.h>
#include <REX/W32/SHELL32.h>
#include <imgui.h>

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace UI::Fonts
{
    namespace
    {
        // In lookup order: the first file that loads is the main font, and each
        // later one only supplies glyphs the earlier ones lack. Windows and
        // Proton ship some of them under different names.
        constexpr std::array<std::wstring_view, 12> kFontFiles{
            L"segoeui.ttf",  // Latin, Greek, Cyrillic, Arabic, Hebrew, Armenian, Georgian
            L"arial.ttf",
            L"tahoma.ttf",  // Arabic, Hebrew, Thai
            L"micross.ttf",
            L"Nirmala.ttc",  // Indic scripts
            L"nirmala.ttf",
            L"msyh.ttc",  // Chinese
            L"msyh.ttf",
            L"YuGothM.ttc",  // Japanese
            L"msgothic.ttc",
            L"malgun.ttf",  // Korean
            L"seguisym.ttf",  // Symbols
        };

        // <windows.h> reaches plugin sources through the precompiled header, and
        // its macros hide REX's constants of the same names.
        constexpr std::uint32_t kGenericRead = 0x80000000u;
        constexpr std::uint32_t kFileShareRead = 0x00000001u;
        constexpr std::uint32_t kOpenExisting = 3u;
        constexpr std::uint32_t kPageReadOnly = 0x00000002u;
        constexpr std::uint32_t kFileMapRead = 0x00000004u;
        const auto kInvalidHandle = reinterpret_cast<REX::W32::HANDLE>(static_cast<std::intptr_t>(-1));

        constexpr REX::W32::GUID kFontsFolderID{ 0xFD228CB7u, 0xAE11u, 0x4AE3u,
            { 0x86u, 0x4Cu, 0x16u, 0xF3u, 0x91u, 0x0Au, 0xB8u, 0xFEu } };

        struct HandleCloser
        {
            void operator()(REX::W32::HANDLE a_handle) const noexcept { REX::W32::CloseHandle(a_handle); }
        };
        using UniqueHandle = std::unique_ptr<void, HandleCloser>;

        struct ViewUnmapper
        {
            void operator()(const std::byte* a_view) const noexcept { REX::W32::UnmapViewOfFile(a_view); }
        };
        using MappedView = std::unique_ptr<const std::byte, ViewUnmapper>;

        // The atlas reads glyphs from these views whenever it bakes new ones,
        // and it lives as long as the process.
        std::vector<MappedView> s_views;

        std::optional<std::filesystem::path> GetFontsDirectory()
        {
            wchar_t* path = nullptr;
            const bool found = REX::W32::SHGetKnownFolderPath(kFontsFolderID, 0, nullptr, &path) >= 0 && path;
            std::optional<std::filesystem::path> directory;
            if (found)
            {
                directory.emplace(path);
            }
            REX::W32::CoTaskMemFree(path);
            return directory;
        }

        // Empty if the file is missing or cannot be mapped.
        std::span<const std::byte> MapFile(const std::filesystem::path& a_path)
        {
            std::error_code error;
            const auto size = std::filesystem::file_size(a_path, error);
            if (error || size == 0 || size > static_cast<std::uintmax_t>(INT_MAX))
            {
                return {};
            }

            const auto file = REX::W32::CreateFileW(
                a_path.c_str(), kGenericRead, kFileShareRead, nullptr, kOpenExisting, 0, nullptr);
            if (file == kInvalidHandle)
            {
                return {};
            }
            const UniqueHandle fileOwner{ file };
            // The view keeps the mapping alive after both handles close.
            const UniqueHandle mapping{ REX::W32::CreateFileMappingW(file, nullptr, kPageReadOnly, 0, 0, nullptr) };
            if (!mapping)
            {
                return {};
            }
            MappedView view{ static_cast<const std::byte*>(
                REX::W32::MapViewOfFile(mapping.get(), kFileMapRead, 0, 0, 0)) };
            if (!view)
            {
                return {};
            }

            const std::span<const std::byte> data{ view.get(), static_cast<std::size_t>(size) };
            s_views.push_back(std::move(view));
            return data;
        }
    }

    void Load(ImFontAtlas& a_atlas, float a_sizePixels)
    {
        ImFontConfig config;
        config.SizePixels = a_sizePixels;
        config.FontDataOwnedByAtlas = false;

        std::string loaded;
        if (const auto directory = GetFontsDirectory())
        {
            for (const auto fileName : kFontFiles)
            {
                const auto path = *directory / fileName;
                const auto data = MapFile(path);
                if (data.empty())
                {
                    continue;
                }
                config.MergeMode = !loaded.empty();
                if (!a_atlas.AddFontFromMemoryTTF(const_cast<std::byte*>(data.data()),
                        static_cast<int>(data.size()), a_sizePixels, &config))
                {
                    logs::warn("UI overlay: could not load font {}", path.string());
                    s_views.pop_back();
                    continue;
                }
                loaded += loaded.empty() ? "" : ", ";
                loaded += path.filename().string();
            }
        }
        else
        {
            logs::warn("UI overlay: could not resolve the system fonts folder");
        }

        if (!loaded.empty())
        {
            logs::info("UI overlay: fonts {}", loaded);
            return;
        }

        logs::warn("UI overlay: no system font loaded; text is limited to Latin");
        ImFontConfig defaultConfig;
        defaultConfig.SizePixels = a_sizePixels;
        a_atlas.AddFontDefaultVector(&defaultConfig);
    }
}
