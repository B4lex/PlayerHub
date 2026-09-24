#pragma once

#include <chrono>
#include <cstdint>

// Measures the plugin's own processing time per main-loop pass and logs a
// summary periodically. Compiled out unless the frame_profiler xmake option is
// enabled, in which case every call below is an empty inline.
namespace FrameProfiler
{
    enum class Section : std::uint8_t
    {
        MainLoop,
        Overlay,
        Count
    };

#ifdef SKYRIM_MP_FRAME_PROFILER
    class ScopedSection
    {
    public:
        explicit ScopedSection(Section a_section);
        ~ScopedSection();

        ScopedSection(const ScopedSection&) = delete;
        ScopedSection& operator=(const ScopedSection&) = delete;

    private:
        Section section;
        std::chrono::steady_clock::time_point start;
    };

    // Closes the current frame. Call once per main-loop pass, on that thread.
    void EndFrame();
#else
    class ScopedSection
    {
    public:
        explicit ScopedSection(Section) {}
    };

    inline void EndFrame() {}
#endif
}
