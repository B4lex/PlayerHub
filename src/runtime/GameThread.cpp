#include "runtime/GameThread.h"

#include <REX/W32/KERNEL32.h>
#include <atomic>
#include <cstdint>

namespace GameThread
{
    namespace
    {
        std::atomic_uint32_t s_authoritativeGameThreadID{ 0 };
        std::atomic_bool s_wrongThreadLogged{ false };
        std::atomic_bool s_gameThreadMigrationLogged{ false };
    }

    bool Bind()
    {
        const auto currentThread = REX::W32::GetCurrentThreadId();
        std::uint32_t expectedThread = 0;
        if (s_authoritativeGameThreadID.compare_exchange_strong(
                expectedThread, currentThread, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            logs::debug("Bound remote actor engine access to main-loop thread {}", currentThread);
            return true;
        }

        if (expectedThread == currentThread)
        {
            return true;
        }

        if (!s_gameThreadMigrationLogged.exchange(true, std::memory_order_relaxed))
        {
            logs::critical(
                "Main-loop hook migrated threads; refusing Remote actor update: currentThread={} "
                "authoritativeThread={}",
                currentThread, expectedThread);
        }
        return false;
    }

    bool IsAuthoritative()
    {
        const auto authoritativeThread = s_authoritativeGameThreadID.load(std::memory_order_acquire);
        return authoritativeThread != 0 && authoritativeThread == REX::W32::GetCurrentThreadId();
    }

    bool Require(const char* a_operation)
    {
        const auto currentThread = REX::W32::GetCurrentThreadId();
        const auto authoritativeThread = s_authoritativeGameThreadID.load(std::memory_order_acquire);
        if (authoritativeThread != 0 && authoritativeThread == currentThread)
        {
            return true;
        }

        if (!s_wrongThreadLogged.exchange(true, std::memory_order_relaxed))
        {
            logs::critical(
                "Refused remote actor engine access outside the authoritative main-loop thread: operation={} "
                "currentThread={} authoritativeThread={}",
                a_operation, currentThread, authoritativeThread);
        }
        return false;
    }
}
