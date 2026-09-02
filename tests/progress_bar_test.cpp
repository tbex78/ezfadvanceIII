#include "ezfadvance/progress_bar.hpp"

#include <cassert>
#include <chrono>
#include <sstream>
#include <string>

int main()
{
    using ProgressBar = ezfadvance::ProgressBar;
    auto now = ProgressBar::Clock::time_point{};
    std::ostringstream output;
    {
        ProgressBar progress("Read", 1024, true, true, output,
                             [&now] { return now; });
        now += std::chrono::seconds(1);
        progress.update(512);
        now += std::chrono::seconds(1);
        progress.update(1024);
    }
    const std::string text = output.str();
    assert(text.find("Read [") != std::string::npos);
    assert(text.find("50.0%") != std::string::npos);
    assert(text.find("100.0%") != std::string::npos);
    assert(text.find("0.5 KiB/s") != std::string::npos);
    assert(!text.empty() && text.back() == '\n');

    std::ostringstream disabled;
    ProgressBar quiet("Quiet", 1, false, false, disabled,
                      [] { return ProgressBar::Clock::time_point{}; });
    quiet.update(1);
    assert(disabled.str().empty());
}
