#pragma once

namespace GameThread
{
    [[nodiscard]] bool Bind();

    [[nodiscard]] bool IsAuthoritative();

    [[nodiscard]] bool Require(const char* a_operation);
}
