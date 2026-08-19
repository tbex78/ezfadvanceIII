#pragma once

#include <cstddef>

namespace ezfadvance {

enum class VerificationMode {
    partial_first_window,
    exact_8_mib,
    partial_12_mib,
    exact_16_mib,
    tiny_tail_above_16_mib,
    partial_20_mib,
    exact_24_mib,
    exact_32_mib,
    unsupported_partial_higher_window
};

class VerificationPolicy final {
public:
    static constexpr std::size_t block_size = 0x10000;
    static constexpr std::size_t window_size = 0x800000;
    static constexpr std::size_t size_12_mib = 0xC00000;
    static constexpr std::size_t size_16_mib = 0x1000000;
    static constexpr std::size_t size_20_mib = 0x1400000;
    static constexpr std::size_t size_24_mib = 0x1800000;
    static constexpr std::size_t size_32_mib = 0x2000000;

    VerificationMode modeFor(std::size_t image_size) const noexcept;
};

} // namespace ezfadvance
