#include "ezfadvance/progress_bar.hpp"

#include "ezfadvance/platform.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>

namespace ezfadvance {

ProgressBar::ProgressBar(std::string label, std::uint64_t total,
                         bool byte_units, bool enabled)
    : ProgressBar(std::move(label), total, byte_units, enabled,
                  std::cout, [] { return Clock::now(); })
{
}

ProgressBar::ProgressBar(std::string label, std::uint64_t total,
                         bool byte_units, bool enabled,
                         std::ostream& output, Now now)
    : label_(std::move(label)),
      total_(total),
      byte_units_(byte_units),
      enabled_(enabled),
      output_(output),
      now_(std::move(now)),
      started_(now_())
{
}

ProgressBar::~ProgressBar()
{
    if (enabled_ && drew_ && !finished_) output_ << '\n';
}

std::string ProgressBar::duration(double seconds)
{
    if (seconds < 0.0) seconds = 0.0;
    const auto total_seconds = static_cast<std::uint64_t>(seconds + 0.5);
    const auto hours = total_seconds / 3600;
    const auto minutes = (total_seconds % 3600) / 60;
    const auto secs = total_seconds % 60;
    std::ostringstream output;
    if (hours != 0) {
        output << hours << ':' << std::setw(2) << std::setfill('0') << minutes
               << ':' << std::setw(2) << secs;
    } else {
        output << minutes << ':' << std::setw(2) << std::setfill('0') << secs;
    }
    return output.str();
}

void ProgressBar::update(std::uint64_t completed)
{
    if (!enabled_) return;
    completed = std::min(completed, total_);

    const double elapsed =
        std::chrono::duration<double>(now_() - started_).count();
    const double ratio = total_ == 0 ? 1.0 :
        static_cast<double>(completed) / static_cast<double>(total_);
    static constexpr std::size_t width = 32;
    const auto filled = std::min<std::size_t>(
        static_cast<std::size_t>(ratio * width), width);

    std::ostringstream line_builder;
    line_builder << label_ << " [";
    for (std::size_t i = 0; i < width; ++i) {
        line_builder << (i < filled ? '=' :
            (i == filled && completed < total_ ? '>' : ' '));
    }
    line_builder << "] " << std::fixed << std::setprecision(1)
                 << ratio * 100.0 << "%  ";

    if (byte_units_) {
        line_builder << std::setprecision(2)
                     << static_cast<double>(completed) / (1024.0 * 1024.0)
                     << '/'
                     << static_cast<double>(total_) / (1024.0 * 1024.0)
                     << " MiB";
    } else {
        line_builder << completed << '/' << total_;
    }

    if (elapsed > 0.0 && completed != 0) {
        const double rate = static_cast<double>(completed) / elapsed;
        line_builder << "  " << std::setprecision(1);
        if (byte_units_) line_builder << rate / 1024.0 << " KiB/s";
        else line_builder << rate << "/s";
        const double remaining = rate > 0.0 ?
            static_cast<double>(total_ - completed) / rate : 0.0;
        line_builder << "  elapsed " << duration(elapsed)
                     << "  ETA " << duration(remaining);
    }

    const std::string line = fitProgressToTerminal(line_builder.str());
    beginProgressLine(output_);
    output_ << line;
    if (last_width_ > line.size())
        output_ << std::string(last_width_ - line.size(), ' ');
    output_ << std::flush;
    last_width_ = line.size();
    drew_ = true;
    if (completed >= total_) {
        output_ << '\n';
        finished_ = true;
    }
}

} // namespace ezfadvance
