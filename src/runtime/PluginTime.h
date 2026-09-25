#pragma once

#include <chrono>
#include <optional>

// Measures how much time the plugin's own hooks take per main-loop pass.
namespace PluginTime
{
    // Adds the lifetime of the enclosing scope to the plugin's time. Safe to use
    // on any thread.
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

    // Main-loop thread, once per pass after the plugin's work for that pass.
    void EndFrame();

    // Average plugin time per main-loop pass over the latest completed window.
    // Empty until the first window completes. Safe to call from any thread.
    [[nodiscard]] std::optional<std::chrono::nanoseconds> GetAverageFrameTime();
}
