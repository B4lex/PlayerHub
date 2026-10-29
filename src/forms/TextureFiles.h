#pragma once

#include <string_view>

namespace Forms
{
    // Whether a loose file or an archive holds the texture, by a path relative to
    // Data\Textures or to Data with a leading "textures\". Cached per path.
    [[nodiscard]] bool TextureExists(std::string_view a_path);
}
