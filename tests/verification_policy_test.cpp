#include "ezfadvance/verification_policy.hpp"

#include <cassert>

int main()
{
    using ezfadvance::VerificationMode;
    ezfadvance::VerificationPolicy policy;
    assert(policy.modeFor(0x10000) == VerificationMode::partial_first_window);
    assert(policy.modeFor(0x800000) == VerificationMode::exact_8_mib);
    assert(policy.modeFor(0x900000) == VerificationMode::unsupported_partial_higher_window);
    assert(policy.modeFor(0xC00000) == VerificationMode::partial_12_mib);
    assert(policy.modeFor(0xC00001) == VerificationMode::unsupported_partial_higher_window);
    assert(policy.modeFor(0x1000000) == VerificationMode::exact_16_mib);
    assert(policy.modeFor(0x1000001) == VerificationMode::tiny_tail_above_16_mib);
    assert(policy.modeFor(0x1010000) == VerificationMode::tiny_tail_above_16_mib);
    assert(policy.modeFor(0x1010001) == VerificationMode::unsupported_partial_higher_window);
    assert(policy.modeFor(0x13FFFFF) == VerificationMode::unsupported_partial_higher_window);
    assert(policy.modeFor(0x1400000) == VerificationMode::partial_20_mib);
    assert(policy.modeFor(0x1400001) == VerificationMode::unsupported_partial_higher_window);
    assert(policy.modeFor(0x1800000) == VerificationMode::exact_24_mib);
    assert(policy.modeFor(0x1BFFFFF) == VerificationMode::unsupported_partial_higher_window);
    assert(policy.modeFor(0x1C00000) == VerificationMode::partial_28_mib);
    assert(policy.modeFor(0x1C00001) == VerificationMode::unsupported_partial_higher_window);
    assert(policy.modeFor(0x2000000) == VerificationMode::exact_32_mib);
}
