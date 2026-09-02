#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>

namespace ezfadvance {

class ProgressBar final {
public:
    using Clock = std::chrono::steady_clock;
    using Now = std::function<Clock::time_point()>;

    ProgressBar(std::string label, std::uint64_t total,
                bool byte_units = true, bool enabled = true);
    ProgressBar(std::string label, std::uint64_t total,
                bool byte_units, bool enabled,
                std::ostream& output, Now now);
    ~ProgressBar();

    ProgressBar(const ProgressBar&) = delete;
    ProgressBar& operator=(const ProgressBar&) = delete;

    void update(std::uint64_t completed);

private:
    static std::string duration(double seconds);

    std::string label_;
    std::uint64_t total_;
    bool byte_units_;
    bool enabled_;
    std::ostream& output_;
    Now now_;
    Clock::time_point started_;
    bool drew_ = false;
    bool finished_ = false;
    std::size_t last_width_ = 0;
};

} // namespace ezfadvance
