#include "ezfadvance/verification_session.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

namespace ezfadvance {
namespace {

constexpr unsigned usb_timeout_ms = 15000;

void writeLe32(std::vector<std::uint8_t>& bytes, std::size_t offset,
               std::uint32_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

} // namespace

VerificationSession::VerificationSession(Transport& transport)
    : VerificationSession(
          transport,
          [](std::chrono::microseconds duration) {
              std::this_thread::sleep_for(duration);
          })
{
}

VerificationSession::VerificationSession(Transport& transport,
                                         DelayCallback delay)
    : transport_(transport), protocol_(transport), delay_(std::move(delay))
{
    if (!delay_)
        throw std::invalid_argument("verification delay callback is empty");
}

std::size_t VerificationSession::partialFirstWindowExtent(
    std::size_t image_size)
{
    if (image_size == 0 || image_size >= first_window_size)
        throw std::invalid_argument(
            "partial first-window verification requires 0 < size < 8 MiB");
    return (image_size + block_size - 1) & ~(block_size - 1);
}

std::size_t VerificationSession::tinyTailAbove16MiBExtent(
    std::size_t image_size)
{
    if (image_size <= two_window_size ||
        image_size > two_window_size + block_size)
        throw std::invalid_argument(
            "tiny-tail verification requires 16 MiB < size <= 16 MiB + 64 KiB");
    return (image_size + block_size - 1) & ~(block_size - 1);
}

bool VerificationSession::statusSequence() const
{
    // Hardware isolation proved the former one-byte tail addresses save
    // offsets 0 and 1. Verification needs only the two-byte control transfer.
    return protocol_.tx92Two(0xFF, 0xFF, "STATUS FFFF");
}

std::vector<std::uint8_t> VerificationSession::readCommand(
    std::size_t byte_address, std::size_t length)
{
    if ((byte_address & 1u) != 0)
        throw std::invalid_argument("read address must be word-aligned");
    if (byte_address / 2 > std::numeric_limits<std::uint32_t>::max() ||
        length > std::numeric_limits<std::uint32_t>::max())
        throw std::out_of_range("verification read exceeds protocol range");

    std::vector<std::uint8_t> command = {
        0x5A, 0xA5, 0x91, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00
    };
    writeLe32(command, 4, static_cast<std::uint32_t>(byte_address / 2));
    writeLe32(command, 8, static_cast<std::uint32_t>(length));
    return command;
}

bool VerificationSession::compareBlock(
    const std::vector<std::uint8_t>& image,
    std::size_t offset,
    const std::vector<std::uint8_t>& received,
    bool require_erased_padding)
{
    for (std::size_t i = 0; i < received.size(); ++i) {
        const std::size_t absolute = offset + i;
        if (absolute >= image.size() && !require_erased_padding)
            continue;
        const std::uint8_t expected =
            absolute < image.size() ? image[absolute] : 0xFF;
        if (received[i] != expected) {
            std::cerr << "VERIFY FAILED at byte 0x" << std::hex << absolute
                      << ": expected 0x" << static_cast<unsigned>(expected)
                      << " got 0x" << static_cast<unsigned>(received[i])
                      << std::dec << '\n';
            return false;
        }
    }
    return true;
}

bool VerificationSession::verifyPartialFirstWindow(
    const std::vector<std::uint8_t>& image,
    const BlockVerifiedCallback& block_verified) const
{
    const std::size_t verify_size = partialFirstWindowExtent(image.size());
    if (!statusSequence()) return false;

    return verifyLinear(image, verify_size, block_verified);
}

bool VerificationSession::verifyExact8MiB(
    const std::vector<std::uint8_t>& image,
    const BlockVerifiedCallback& block_verified) const
{
    if (image.size() != first_window_size)
        throw std::invalid_argument(
            "exact 8-MiB verification requires an 8-MiB image");

    if (!statusSequence()) return false;

    if (!protocol_.tx92Two(0x55, 0xAA, "VERIFY64 55AA")) return false;
    if (!protocol_.tx92Two(0x02, 0x00, "VERIFY64 0200")) return false;
    if (!protocol_.tx92Two(0x00, 0x40, "VERIFY64 0040")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY64 0000 A")) return false;

    delay_(std::chrono::microseconds(125000));

    if (!protocol_.tx92Two(0xAA, 0x55, "VERIFY64 AA55")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY64 0000 B")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY64 0000 C")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY64 0000 D")) return false;
    if (!statusSequence()) return false;

    if (!protocol_.tx92Two(0x55, 0xAA, "VERIFY64READ 55AA")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY64READ 0000 A")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY64READ 0000 B")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY64READ 0000 C")) return false;

    return verifyLinear(image, image.size(), block_verified);
}

bool VerificationSession::verifyExact16MiB(
    const std::vector<std::uint8_t>& image,
    const BlockVerifiedCallback& block_verified) const
{
    if (image.size() != two_window_size)
        throw std::invalid_argument(
            "exact 16-MiB verification requires a 16-MiB image");

    // writerom128Mb.pcap requires the complete writer-only post-program
    // status tail here. These one-byte transfers touch save bytes; CardWriter
    // clears all four save banks after verification, including on failure.
    if (!protocol_.tx92Two(0xFF, 0xFF, "VERIFY128 status FFFF")) return false;
    if (!protocol_.tx92One(0x01, 0x04, "VERIFY128 status 1/04")) return false;
    if (!protocol_.tx92One(0x00, 0x00, "VERIFY128 status 0/00 A")) return false;
    if (!protocol_.tx92One(0x00, 0x00, "VERIFY128 status 0/00 B")) return false;

    if (!protocol_.tx92Two(0x55, 0xAA, "VERIFY128 55AA")) return false;
    if (!protocol_.tx92Two(0x02, 0x00, "VERIFY128 0200")) return false;
    if (!protocol_.tx92Two(0x00, 0x80, "VERIFY128 0080")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY128 0000 A")) return false;

    delay_(std::chrono::microseconds(125000));

    if (!protocol_.tx92Two(0xAA, 0x55, "VERIFY128 AA55")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY128 0000 B")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY128 0000 C")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY128 0000 D")) return false;
    if (!protocol_.tx92One(0x00, 0xAA, "VERIFY128 selector0 AA")) return false;
    if (!protocol_.tx92One(0x00, 0x55, "VERIFY128 selector0 55")) return false;
    if (!protocol_.tx92One(0x01, 0x06, "VERIFY128 selector1 06")) return false;

    if (!protocol_.tx92Two(0xFF, 0xFF, "VERIFY128 final status FFFF")) return false;
    if (!protocol_.tx92One(0x01, 0x04, "VERIFY128 final status 1/04")) return false;
    if (!protocol_.tx92One(0x00, 0x00, "VERIFY128 final status 0/00 A")) return false;
    if (!protocol_.tx92One(0x00, 0x00, "VERIFY128 final status 0/00 B")) return false;

    if (!protocol_.tx92Two(0x55, 0xAA, "VERIFY128READ 55AA")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY128READ 0000 A")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY128READ 0000 B")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY128READ 0000 C")) return false;

    return verifyLinear(image, image.size(), block_verified);
}

bool VerificationSession::verifyPartial12MiB(
    const std::vector<std::uint8_t>& image,
    const BlockVerifiedCallback& block_verified) const
{
    constexpr std::size_t image_size = 0xC00000;
    if (image.size() != image_size)
        throw std::invalid_argument(
            "partial 12-MiB verification requires a 12-MiB image");

    // 8_4MB.pcap sends this complete post-program status sequence before its
    // first linear read. Hardware testing proved that omitting its one-byte
    // tail leaves the upper flash window visible at logical address zero.
    // The one-byte transfers also touch save offsets 0 and 1, so this method
    // is writer-only: CardWriter unconditionally clears all four save banks
    // after verification, including on verification failure.
    if (!protocol_.tx92Two(0xFF, 0xFF, "VERIFY96 status FFFF")) return false;
    if (!protocol_.tx92One(0x01, 0x04, "VERIFY96 status 1/04")) return false;
    if (!protocol_.tx92One(0x00, 0x00, "VERIFY96 status 0/00 A")) return false;
    if (!protocol_.tx92One(0x00, 0x00, "VERIFY96 status 0/00 B")) return false;
    if (!protocol_.tx92Two(0x55, 0xAA, "VERIFY96READ 55AA")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY96READ 0000 A")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY96READ 0000 B")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY96READ 0000 C")) return false;

    return verifyLinear(image, image.size(), block_verified);
}

bool VerificationSession::verifyTinyTailAbove16MiB(
    const std::vector<std::uint8_t>& image,
    const BlockVerifiedCallback& block_verified) const
{
    const std::size_t verify_size = tinyTailAbove16MiBExtent(image.size());

    // fireemblem.pcap programs the tiny tail after selecting the second
    // window, then requires only this complete writer-only status tail before
    // its linear reads. CardWriter clears all save banks afterward.
    if (!protocol_.tx92Two(0xFF, 0xFF, "VERIFYTAIL status FFFF")) return false;
    if (!protocol_.tx92One(0x01, 0x04, "VERIFYTAIL status 1/04")) return false;
    if (!protocol_.tx92One(0x00, 0x00, "VERIFYTAIL status 0/00 A")) return false;
    if (!protocol_.tx92One(0x00, 0x00, "VERIFYTAIL status 0/00 B")) return false;
    if (!protocol_.tx92Two(0x55, 0xAA, "VERIFYTAIL 55AA")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFYTAIL 0000 A")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFYTAIL 0000 B")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFYTAIL 0000 C")) return false;

    // fireemblem.pcap issues a full 64-KiB final read although its captured
    // tail erase covers only the small sector containing the programmed
    // loader. Compare the complete constructed image, but do not infer that
    // later, unprogrammed sectors must contain 0xFF.
    return verifyLinear(image, verify_size, block_verified, false);
}

bool VerificationSession::verifyExact24MiB(
    const std::vector<std::uint8_t>& image,
    const BlockVerifiedCallback& block_verified) const
{
    if (image.size() != three_window_size)
        throw std::invalid_argument(
            "exact 24-MiB verification requires a 24-MiB image");

    // 4_4_4_4_8MB.pcap requires the complete writer-only post-program
    // status tail. CardWriter clears all four save banks afterward.
    if (!protocol_.tx92Two(0xFF, 0xFF, "VERIFY192 status FFFF")) return false;
    if (!protocol_.tx92One(0x01, 0x04, "VERIFY192 status 1/04")) return false;
    if (!protocol_.tx92One(0x00, 0x00, "VERIFY192 status 0/00 A")) return false;
    if (!protocol_.tx92One(0x00, 0x00, "VERIFY192 status 0/00 B")) return false;

    if (!protocol_.tx92Two(0x55, 0xAA, "VERIFY192 55AA")) return false;
    if (!protocol_.tx92Two(0x02, 0x00, "VERIFY192 0200")) return false;
    if (!protocol_.tx92Two(0x00, 0xC0, "VERIFY192 00C0")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY192 0000 A")) return false;

    delay_(std::chrono::microseconds(125000));

    if (!protocol_.tx92Two(0xAA, 0x55, "VERIFY192 AA55")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY192 0000 B")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY192 0000 C")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY192 0000 D")) return false;
    if (!protocol_.tx92One(0x00, 0xAA, "VERIFY192 selector0 AA")) return false;
    if (!protocol_.tx92One(0x00, 0x55, "VERIFY192 selector0 55")) return false;
    if (!protocol_.tx92One(0x01, 0x06, "VERIFY192 selector1 06")) return false;

    if (!protocol_.tx92Two(0xFF, 0xFF, "VERIFY192 final status FFFF")) return false;
    if (!protocol_.tx92One(0x01, 0x04, "VERIFY192 final status 1/04")) return false;
    if (!protocol_.tx92One(0x00, 0x00, "VERIFY192 final status 0/00 A")) return false;
    if (!protocol_.tx92One(0x00, 0x00, "VERIFY192 final status 0/00 B")) return false;

    if (!protocol_.tx92Two(0x55, 0xAA, "VERIFY192READ 55AA")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY192READ 0000 A")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY192READ 0000 B")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY192READ 0000 C")) return false;

    return verifyLinear(image, image.size(), block_verified);
}

bool VerificationSession::verifyPartial20MiB(
    const std::vector<std::uint8_t>& image,
    const BlockVerifiedCallback& block_verified) const
{
    constexpr std::size_t image_size = 0x1400000;
    if (image.size() != image_size)
        throw std::invalid_argument(
            "partial 20-MiB verification requires a 20-MiB image");

    // 16_4MB.pcap and 4_8_8MB.pcap agree on this complete writer-only
    // post-program status tail. CardWriter clears all save banks afterward.
    if (!protocol_.tx92Two(0xFF, 0xFF, "VERIFY160 status FFFF")) return false;
    if (!protocol_.tx92One(0x01, 0x04, "VERIFY160 status 1/04")) return false;
    if (!protocol_.tx92One(0x00, 0x00, "VERIFY160 status 0/00 A")) return false;
    if (!protocol_.tx92One(0x00, 0x00, "VERIFY160 status 0/00 B")) return false;
    if (!protocol_.tx92Two(0x55, 0xAA, "VERIFY160READ 55AA")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY160READ 0000 A")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY160READ 0000 B")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY160READ 0000 C")) return false;

    return verifyLinear(image, image.size(), block_verified);
}

bool VerificationSession::verifyExact32MiB(
    const std::vector<std::uint8_t>& image,
    const BlockVerifiedCallback& block_verified) const
{
    if (image.size() != four_window_size)
        throw std::invalid_argument(
            "exact 32-MiB verification requires a 32-MiB image");

    // 256MBits-rom.pcap transitions from the already-selected final program
    // window, but still requires the complete writer-only status tails.
    // CardWriter clears all four save banks afterward.
    if (!protocol_.tx92Two(0xFF, 0xFF, "VERIFY256 status FFFF")) return false;
    if (!protocol_.tx92One(0x01, 0x04, "VERIFY256 status 1/04")) return false;
    if (!protocol_.tx92One(0x00, 0x00, "VERIFY256 status 0/00 A")) return false;
    if (!protocol_.tx92One(0x00, 0x00, "VERIFY256 status 0/00 B")) return false;
    if (!protocol_.tx92Two(0x55, 0xAA, "VERIFY256 55AA")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY256 0000 A")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY256 0000 B")) return false;
    if (!protocol_.tx92Two(0x02, 0x00, "VERIFY256 0200")) return false;

    delay_(std::chrono::microseconds(125000));

    if (!protocol_.tx92Two(0xAA, 0x55, "VERIFY256 AA55")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY256 0000 C")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY256 0000 D")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY256 0000 E")) return false;
    if (!protocol_.tx92One(0x00, 0xAA, "VERIFY256 selector0 AA")) return false;
    if (!protocol_.tx92One(0x00, 0x55, "VERIFY256 selector0 55")) return false;
    if (!protocol_.tx92One(0x01, 0x06, "VERIFY256 selector1 06")) return false;

    if (!protocol_.tx92Two(0xFF, 0xFF, "VERIFY256 final status FFFF")) return false;
    if (!protocol_.tx92One(0x01, 0x04, "VERIFY256 final status 1/04")) return false;
    if (!protocol_.tx92One(0x00, 0x00, "VERIFY256 final status 0/00 A")) return false;
    if (!protocol_.tx92One(0x00, 0x00, "VERIFY256 final status 0/00 B")) return false;
    if (!protocol_.tx92Two(0x55, 0xAA, "VERIFY256READ 55AA")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY256READ 0000 A")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY256READ 0000 B")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY256READ 0000 C")) return false;

    return verifyLinear(image, image.size(), block_verified);
}

bool VerificationSession::verifyPartial28MiB(
    const std::vector<std::uint8_t>& image,
    const BlockVerifiedCallback& block_verified) const
{
    constexpr std::size_t image_size = 0x1C00000;
    if (image.size() != image_size)
        throw std::invalid_argument(
            "partial 28-MiB verification requires a 28-MiB image");

    // 4_8_16MB.pcap requires this complete writer-only post-program status
    // tail. CardWriter clears all four save banks afterward.
    if (!protocol_.tx92Two(0xFF, 0xFF, "VERIFY224 status FFFF")) return false;
    if (!protocol_.tx92One(0x01, 0x04, "VERIFY224 status 1/04")) return false;
    if (!protocol_.tx92One(0x00, 0x00, "VERIFY224 status 0/00 A")) return false;
    if (!protocol_.tx92One(0x00, 0x00, "VERIFY224 status 0/00 B")) return false;
    if (!protocol_.tx92Two(0x55, 0xAA, "VERIFY224READ 55AA")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY224READ 0000 A")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY224READ 0000 B")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY224READ 0000 C")) return false;

    return verifyLinear(image, image.size(), block_verified);
}

bool VerificationSession::verifyLinear(
    const std::vector<std::uint8_t>& image,
    std::size_t verify_size,
    const BlockVerifiedCallback& block_verified,
    bool require_erased_padding) const
{
    for (std::size_t offset = 0; offset < verify_size; ) {
        const std::size_t length =
            std::min(block_size, verify_size - offset);
        if (!transport_.out(readCommand(offset, length), usb_timeout_ms))
            return false;

        std::vector<std::uint8_t> received;
        if (!transport_.inExact(received, length, usb_timeout_ms))
            return false;
        if (!compareBlock(image, offset, received, require_erased_padding))
            return false;

        if (block_verified) block_verified(offset, length);
        offset += length;
    }
    return true;
}

} // namespace ezfadvance
