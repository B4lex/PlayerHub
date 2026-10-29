#include "forms/TextureFiles.h"
#include "runtime/GameThread.h"
#include "text/AsciiCase.h"

#include <RE/Skyrim.h>

#include <cstddef>
#include <string>
#include <unordered_map>

namespace Forms
{
    namespace
    {
        // Peers can send any valid path, so a stream of missing ones must not grow the
        // cache without end.
        constexpr std::size_t kMaximumCachedPaths = 4096;
        constexpr std::string_view kTexturesFolder = "textures\\";

        std::unordered_map<std::string, bool> s_exists;
    }

    bool TextureExists(std::string_view a_path)
    {
        if (!GameThread::Require("TextureExists"))
        {
            return false;
        }

        std::string path;
        path.reserve(kTexturesFolder.size() + a_path.size());
        if (!AsciiCase::StartsWith(a_path, kTexturesFolder) && !AsciiCase::StartsWith(a_path, "textures/"))
        {
            path = kTexturesFolder;
        }
        for (const auto character : a_path)
        {
            path.push_back(character == '/' ? '\\' : AsciiCase::ToLower(character));
        }

        if (const auto found = s_exists.find(path); found != s_exists.end())
        {
            return found->second;
        }
        if (s_exists.size() >= kMaximumCachedPaths)
        {
            s_exists.clear();
        }
        const RE::BSResourceNiBinaryStream stream{ path };
        const bool exists = stream.good();
        s_exists.emplace(std::move(path), exists);
        return exists;
    }
}
