#include "network/TrafficMeter.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace Network
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr auto kRateWindow = std::chrono::seconds{ 1 };
        constexpr auto kStaleWindow = std::chrono::seconds{ 3 };

        std::atomic_uint64_t receivedBytes{ 0 };
        std::atomic_uint64_t sentBytes{ 0 };

        struct Window
        {
            Clock::time_point start;
            std::uint64_t receivedBytes = 0;
            std::uint64_t sentBytes = 0;
        };

        std::mutex ratesLock;
        std::optional<Window> window;
        std::optional<TrafficRates> rates;
    }

    void CountReceivedBytes(std::size_t a_bytes)
    {
        receivedBytes.fetch_add(a_bytes, std::memory_order_relaxed);
    }

    void CountSentBytes(std::size_t a_bytes)
    {
        sentBytes.fetch_add(a_bytes, std::memory_order_relaxed);
    }

    std::optional<TrafficRates> GetTrafficRates()
    {
        const Window now{ .start = Clock::now(),
            .receivedBytes = receivedBytes.load(std::memory_order_relaxed),
            .sentBytes = sentBytes.load(std::memory_order_relaxed) };

        const std::scoped_lock lock{ ratesLock };
        if (!window)
        {
            window = now;
            return rates;
        }

        const auto elapsed = now.start - window->start;
        if (elapsed < kRateWindow)
        {
            return rates;
        }

        if (elapsed >= kStaleWindow)
        {
            rates.reset();
        }
        else
        {
            const std::chrono::duration<double> seconds = elapsed;
            const auto received = static_cast<double>(now.receivedBytes - window->receivedBytes);
            const auto sent = static_cast<double>(now.sentBytes - window->sentBytes);
            rates = TrafficRates{ .receivedBytesPerSecond = received / seconds.count(),
                .sentBytesPerSecond = sent / seconds.count() };
        }
        window = now;
        return rates;
    }
}
