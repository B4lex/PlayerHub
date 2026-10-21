#include "ui/Fonts.h"
#include "config/PluginConfig.h"
#include "text/AsciiCase.h"
#include "text/Text.h"

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
        // Windows and Proton ship some of these under different names.
        constexpr std::array<std::wstring_view, 6> kScriptFiles{
            L"segoeui.ttf",  // Latin, Greek, Cyrillic, Arabic, Hebrew, Armenian, Georgian
            L"arial.ttf",
            L"tahoma.ttf",  // Arabic, Hebrew, Thai
            L"micross.ttf",
            L"Nirmala.ttc",  // Indic scripts
            L"nirmala.ttf",
        };
        // Han characters are drawn by the first of these that has them, and their shapes
        // differ between Simplified Chinese, Traditional Chinese, and Japanese.
        constexpr std::array<std::wstring_view, 2> kSimplifiedChineseFiles{ L"msyh.ttc", L"msyh.ttf" };
        constexpr std::array<std::wstring_view, 1> kTraditionalChineseFiles{ L"msjh.ttc" };
        constexpr std::array<std::wstring_view, 2> kJapaneseFiles{ L"YuGothM.ttc", L"msgothic.ttc" };
        constexpr std::array<std::wstring_view, 2> kLastFiles{
            L"malgun.ttf",  // Korean
            L"seguisym.ttf",  // Symbols
        };

        // Whose Han fonts come first.
        enum class HanOrder
        {
            kSimplifiedChinese,
            kTraditionalChinese,
            kJapanese
        };

        // From the language's gettext code: Chinese is Traditional for Taiwan, Hong Kong,
        // Macau, and the Hant script, and Simplified otherwise, as for every other language.
        HanOrder GetHanOrder(std::string_view a_code)
        {
            if (AsciiCase::StartsWith(a_code, "ja"))
            {
                return HanOrder::kJapanese;
            }
            if (AsciiCase::StartsWith(a_code, "zh")
                && (AsciiCase::Contains(a_code, "_TW") || AsciiCase::Contains(a_code, "_HK")
                    || AsciiCase::Contains(a_code, "_MO") || AsciiCase::Contains(a_code, "Hant")))
            {
                return HanOrder::kTraditionalChinese;
            }
            return HanOrder::kSimplifiedChinese;
        }

        HanOrder GetCurrentHanOrder()
        {
            const auto languages = Text::GetLanguages();
            const auto language = static_cast<std::size_t>(Text::GetLanguage());
            return language < languages.size() ? GetHanOrder(languages[language].code) : HanOrder::kSimplifiedChinese;
        }

        // The text's language puts its own Han font first.
        std::vector<std::wstring_view> GetSystemFiles(HanOrder a_order)
        {
            std::vector<std::wstring_view> files{ kScriptFiles.begin(), kScriptFiles.end() };
            std::array<std::span<const std::wstring_view>, 3> han{ kSimplifiedChineseFiles, kTraditionalChineseFiles,
                kJapaneseFiles };
            if (a_order == HanOrder::kJapanese)
            {
                han = { kJapaneseFiles, kSimplifiedChineseFiles, kTraditionalChineseFiles };
            }
            else if (a_order == HanOrder::kTraditionalChinese)
            {
                han = { kTraditionalChineseFiles, kSimplifiedChineseFiles, kJapaneseFiles };
            }
            for (const auto group : han)
            {
                files.insert(files.end(), group.begin(), group.end());
            }
            files.insert(files.end(), kLastFiles.begin(), kLastFiles.end());
            return files;
        }

        // Sovngarde, under the SIL Open Font License beside the files.
        constexpr std::wstring_view kShippedDirectory = L"PlayerHub/fonts";
        constexpr std::wstring_view kTextFile = L"Sovngarde Light.ttf";
        constexpr std::wstring_view kHeadingFile = L"Sovngarde Bold.ttf";
        constexpr std::wstring_view kLogoFile = L"PlayerHub.ttf";
        // Sovngarde's letters are small for its line height.
        constexpr float kShippedGlyphScale = 1.25F;

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

        ImFont* s_text = nullptr;
        ImFont* s_heading = nullptr;
        ImFont* s_logo = nullptr;
        HanOrder s_loadedHanOrder{ HanOrder::kSimplifiedChinese };

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

        ImFont* AddSource(ImFontAtlas& a_atlas, float a_sizePixels, std::span<const std::byte> a_data, bool a_merge,
            float a_glyphScale = 1.0F)
        {
            ImFontConfig config;
            config.SizePixels = a_sizePixels;
            config.FontDataOwnedByAtlas = false;
            config.MergeMode = a_merge;
            config.ExtraSizeScale = a_glyphScale;
            return a_atlas.AddFontFromMemoryTTF(
                const_cast<std::byte*>(a_data.data()), static_cast<int>(a_data.size()), a_sizePixels, &config);
        }

        ImFont* AddShipped(ImFontAtlas& a_atlas, float a_sizePixels, const std::filesystem::path& a_path,
            std::span<const std::span<const std::byte>> a_fallbacks)
        {
            const auto data = MapFile(a_path);
            if (data.empty())
            {
                logs::warn("UI overlay: could not read font {}; using the system font", a_path.string());
                return nullptr;
            }
            const auto font = AddSource(a_atlas, a_sizePixels, data, false, kShippedGlyphScale);
            if (!font)
            {
                logs::warn("UI overlay: could not load font {}; using the system font", a_path.string());
                s_views.pop_back();
                return nullptr;
            }
            for (const auto fallback : a_fallbacks)
            {
                AddSource(a_atlas, a_sizePixels, fallback, true);
            }
            logs::debug("UI overlay: font {}", a_path.filename().string());
            return font;
        }

        ImFont* AddLogo(ImFontAtlas& a_atlas, float a_sizePixels, const std::filesystem::path& a_path)
        {
            const auto data = MapFile(a_path);
            if (data.empty())
            {
                logs::warn("UI overlay: could not read logo {}; using a text title", a_path.string());
                return nullptr;
            }
            const auto font = AddSource(a_atlas, a_sizePixels, data, false);
            if (!font)
            {
                logs::warn("UI overlay: could not load logo {}; using a text title", a_path.string());
                s_views.pop_back();
                return nullptr;
            }
            logs::debug("UI overlay: logo {}", a_path.filename().string());
            return font;
        }
    }

    void Load(ImFontAtlas& a_atlas, float a_sizePixels)
    {
        std::vector<std::span<const std::byte>> system;
        std::string loaded;
        s_loadedHanOrder = GetCurrentHanOrder();
        if (const auto directory = GetFontsDirectory())
        {
            for (const auto fileName : GetSystemFiles(s_loadedHanOrder))
            {
                const auto path = *directory / fileName;
                const auto data = MapFile(path);
                if (data.empty())
                {
                    continue;
                }
                if (!AddSource(a_atlas, a_sizePixels, data, !system.empty()))
                {
                    logs::warn("UI overlay: could not load font {}", path.string());
                    s_views.pop_back();
                    continue;
                }
                system.push_back(data);
                loaded += loaded.empty() ? "" : ", ";
                loaded += path.filename().string();
            }
        }
        else
        {
            logs::warn("UI overlay: could not resolve the system fonts folder");
        }

        if (system.empty())
        {
            logs::warn("UI overlay: no system font loaded; text is limited to Latin");
            ImFontConfig defaultConfig;
            defaultConfig.SizePixels = a_sizePixels;
            a_atlas.AddFontDefaultVector(&defaultConfig);
        }
        else
        {
            logs::info("UI overlay: fonts {}", loaded);
        }

        const auto directory = Config::GetDirectory();
        if (directory.empty())
        {
            return;
        }
        const auto shipped = directory / kShippedDirectory;
        s_text = AddShipped(a_atlas, a_sizePixels, shipped / kTextFile, system);
        s_heading = AddShipped(a_atlas, a_sizePixels, shipped / kHeadingFile, system);
        s_logo = AddLogo(a_atlas, a_sizePixels, shipped / kLogoFile);
    }

    void Update(ImFontAtlas& a_atlas, float a_sizePixels)
    {
        if (GetCurrentHanOrder() == s_loadedHanOrder)
        {
            return;
        }
        // Clear, not ClearFonts: the latter packs new glyphs into the same texture without
        // erasing it, and the renderer uploads only each glyph's rectangle, so the old
        // glyphs' pixels around them show at their edges. Clear starts a new texture.
        // Either drops the atlas's sources before their views are unmapped.
        a_atlas.Clear();
        s_views.clear();
        s_text = nullptr;
        s_heading = nullptr;
        s_logo = nullptr;
        Load(a_atlas, a_sizePixels);
    }

    ImFont* GetText()
    {
        return s_text;
    }

    ImFont* GetHeading()
    {
        return s_heading;
    }

    ImFont* GetLogo()
    {
        return s_logo;
    }
}
