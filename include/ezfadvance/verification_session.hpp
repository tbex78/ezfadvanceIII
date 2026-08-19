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
    static constexpr std::size_t two_window_size = 0x1000000;
    static constexpr std::size_t three_window_size = 0x1800000;
    static constexpr std::size_t four_window_size = 0x2000000;

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

    // 8_4MB.pcap: status/reset followed by 55AA + three 0000 writes, then
    // 192 global-linear 0x91 reads. No selector tail or delay is emitted.
    bool verifyPartial12MiB(
        const std::vector<std::uint8_t>& image,
        const BlockVerifiedCallback& block_verified = {}) const;

    // writerom128Mb.pcap: explicit 0x0080 mapping transition followed by
    // 256 global-linear 0x91 reads.
    bool verifyExact16MiB(
        const std::vector<std::uint8_t>& image,
        const BlockVerifiedCallback& block_verified = {}) const;

    // fireemblem.pcap: short status/read prefix with no mapping transition or
    // delay, followed by global-linear reads through one rounded tail block.
    bool verifyTinyTailAbove16MiB(
        const std::vector<std::uint8_t>& image,
        const BlockVerifiedCallback& block_verified = {}) const;

    // 4_4_4_4_8MB.pcap: explicit 0x00C0 mapping transition followed by
    // 384 global-linear 0x91 reads.
    bool verifyExact24MiB(
        const std::vector<std::uint8_t>& image,
        const BlockVerifiedCallback& block_verified = {}) const;

    // 256MBits-rom.pcap: transition from the already-selected 0x00C0 program
    // window without emitting another window selector, then 512 linear reads.
    bool verifyExact32MiB(
        const std::vector<std::uint8_t>& image,
        const BlockVerifiedCallback& block_verified = {}) const;

    static std::size_t partialFirstWindowExtent(std::size_t image_size);
    static std::size_t tinyTailAbove16MiBExtent(std::size_t image_size);

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
