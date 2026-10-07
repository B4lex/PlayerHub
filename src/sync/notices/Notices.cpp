#include "sync/notices/Notices.h"

#include <mutex>
#include <utility>

namespace Notices
{
    namespace
    {
        // The overlay drains every frame; without one, as in VR, only the newest are kept.
        constexpr std::size_t kMaximumPending = 32;

        std::mutex s_lock;
        std::vector<std::string> s_pending;
    }

    void Post(std::string a_text)
    {
        logs::debug("Notice: {}", a_text);
        const std::scoped_lock lock{ s_lock };
        if (s_pending.size() >= kMaximumPending)
        {
            s_pending.erase(s_pending.begin());
        }
        s_pending.push_back(std::move(a_text));
    }

    void Drain(std::vector<std::string>& a_notices)
    {
        const std::scoped_lock lock{ s_lock };
        for (auto& notice : s_pending)
        {
            a_notices.push_back(std::move(notice));
        }
        s_pending.clear();
    }
}
