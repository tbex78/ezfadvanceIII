#include "ezfadvance/platform.hpp"

#include <cassert>
#include <sstream>
#include <string>

int main()
{
    const std::string platform = ezfadvance::hostPlatformName();
    assert(!platform.empty());
#if defined(_WIN32)
    assert(platform == "Windows");
#elif defined(__APPLE__) && defined(__MACH__)
    assert(platform == "macOS");
#elif defined(__linux__)
    assert(platform == "Linux");
#endif

    std::ostringstream output;
    ezfadvance::beginProgressLine(output);
    assert(!output.str().empty() && output.str().front() == '\r');
}
