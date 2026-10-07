#pragma once

#include <string>
#include <vector>

namespace Notices
{
    // Any thread.
    void Post(std::string a_text);

    void Drain(std::vector<std::string>& a_notices);
}
