#include "ezfadvance/verification_policy.hpp"

namespace ezfadvance {

VerificationMode VerificationPolicy::modeFor(std::size_t size) const noexcept
{
    if (size == size_32_mib) return VerificationMode::exact_32_mib;
    if (size == size_24_mib) return VerificationMode::exact_24_mib;
    if (size == size_20_mib) return VerificationMode::partial_20_mib;
    if (size == size_16_mib) return VerificationMode::exact_16_mib;
    if (size == size_12_mib) return VerificationMode::partial_12_mib;
    if (size == window_size) return VerificationMode::exact_8_mib;
    if (size < window_size) return VerificationMode::partial_first_window;
    if (size > size_16_mib && size <= size_16_mib + block_size)
        return VerificationMode::tiny_tail_above_16_mib;
    return VerificationMode::unsupported_partial_higher_window;
}

} // namespace ezfadvance
