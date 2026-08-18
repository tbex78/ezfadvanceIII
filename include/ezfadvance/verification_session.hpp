#pragma once

#include "ezfadvance/protocol.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace ezfadvance {

// Capture-derived post-program verification. Paths remain explicit; policy
// selection stays in VerificationPolicy and the writer application.
class VerificationSession final {
public:
    static constexpr std::size_t block_size = 0x10000;
    static constexpr std::size_t first_window_size = 0x800000;

    using BlockVerifiedCallback =
        std::function<void(std::size_t offset, std::size_t length)>;
    using DelayCallback =
        std::function<void(std::chrono::microseconds duration)>;

    explicit VerificationSession(Transport& transport);
    VerificationSession(Transport& transport, DelayCallback delay);

    // 2MB.pcap, 2_2MB.pcap, and 4MB.pcap: status/reset only, followed by
    // global-linear 0x91 reads. No mapping selector transition is emitted.
    bool verifyPartialFirstWindow(
        const std::vector<std::uint8_t>& image,
        const BlockVerifiedCallback& block_verified = {}) const;

    // 4MiB-4MiB.pcap and FLASH_V121_FLASH512K.pcap: explicit 0x0040
    // mapping transition followed by global-linear 0x91 reads.
    bool verifyExact8MiB(
        const std::vector<std::uint8_t>& image,
        const BlockVerifiedCallback& block_verified = {}) const;

    static std::size_t partialFirstWindowExtent(std::size_t image_size);

private:
    bool statusSequence() const;
    static std::vector<std::uint8_t> readCommand(std::size_t byte_address,
                                                  std::size_t length);
    static bool compareBlock(const std::vector<std::uint8_t>& image,
                             std::size_t offset,
                             const std::vector<std::uint8_t>& received);
    bool verifyLinear(const std::vector<std::uint8_t>& image,
                      std::size_t verify_size,
                      const BlockVerifiedCallback& block_verified) const;

    Transport& transport_;
    Protocol protocol_;
    DelayCallback delay_;
};

} // namespace ezfadvance
