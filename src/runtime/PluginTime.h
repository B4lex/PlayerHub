#pragma once

#include <chrono>
#include <optional>

namespace PluginTime
{
    class Scope
    {
    public:
        Scope();
        ~Scope();

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        std::chrono::steady_clock::time_point start;
    };

    void EndFrame();

    [[nodiscard]] std::optional<std::chrono::nanoseconds> GetAverageFrameTime();
}
