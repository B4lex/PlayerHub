#pragma once

#ifdef TRACY_ENABLE
#    include <tracy/Tracy.hpp>

#    include <cstdint>

#    define PROFILE_FUNCTION() ZoneScoped
#    define PROFILE_SCOPE(a_name) ZoneScopedN(a_name)
#    define PROFILE_FRAME() FrameMark
#    define PROFILE_PLOT(a_name, a_value) TracyPlot(a_name, static_cast<std::int64_t>(a_value))
#    define PROFILE_THREAD_NAME(a_name) tracy::SetThreadName(a_name)
#    define PROFILE_LOCKABLE(a_type, a_name) TracyLockable(a_type, a_name)
#    define PROFILE_SHARED_LOCKABLE(a_type, a_name) TracySharedLockable(a_type, a_name)
#else
#    define PROFILE_FUNCTION()
#    define PROFILE_SCOPE(a_name)
#    define PROFILE_FRAME()
#    define PROFILE_PLOT(a_name, a_value)
#    define PROFILE_THREAD_NAME(a_name)
#    define PROFILE_LOCKABLE(a_type, a_name) a_type a_name
#    define PROFILE_SHARED_LOCKABLE(a_type, a_name) a_type a_name
#endif
