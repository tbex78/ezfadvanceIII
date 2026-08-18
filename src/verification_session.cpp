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
    if (!protocol_.tx92Two(0xFF, 0xFF, "STATUS FFFF")) return false;
    if (!protocol_.tx92One(0x01, 0x04, "STATUS 04")) return false;
    if (!protocol_.tx92One(0x00, 0x00, "STATUS 00 A")) return false;
    if (!protocol_.tx92One(0x00, 0x00, "STATUS 00 B")) return false;
    return true;
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
    const std::vector<std::uint8_t>& received)
{
    for (std::size_t i = 0; i < received.size(); ++i) {
        const std::size_t absolute = offset + i;
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
    if (!protocol_.tx92One(0x00, 0xAA, "VERIFY64 selector0 AA")) return false;
    if (!protocol_.tx92One(0x00, 0x55, "VERIFY64 selector0 55")) return false;
    if (!protocol_.tx92One(0x01, 0x06, "VERIFY64 selector1 06")) return false;

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

    if (!statusSequence()) return false;

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

    if (!statusSequence()) return false;

    if (!protocol_.tx92Two(0x55, 0xAA, "VERIFY128READ 55AA")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY128READ 0000 A")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY128READ 0000 B")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY128READ 0000 C")) return false;

    return verifyLinear(image, image.size(), block_verified);
}

bool VerificationSession::verifyTinyTailAbove16MiB(
    const std::vector<std::uint8_t>& image,
    const BlockVerifiedCallback& block_verified) const
{
    const std::size_t verify_size = tinyTailAbove16MiBExtent(image.size());

    if (!statusSequence()) return false;
    if (!protocol_.tx92Two(0x55, 0xAA, "VERIFYTAIL 55AA")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFYTAIL 0000 A")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFYTAIL 0000 B")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFYTAIL 0000 C")) return false;

    return verifyLinear(image, verify_size, block_verified);
}

bool VerificationSession::verifyExact24MiB(
    const std::vector<std::uint8_t>& image,
    const BlockVerifiedCallback& block_verified) const
{
    if (image.size() != three_window_size)
        throw std::invalid_argument(
            "exact 24-MiB verification requires a 24-MiB image");

    if (!statusSequence()) return false;

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

    if (!statusSequence()) return false;

    if (!protocol_.tx92Two(0x55, 0xAA, "VERIFY192READ 55AA")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY192READ 0000 A")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY192READ 0000 B")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY192READ 0000 C")) return false;

    return verifyLinear(image, image.size(), block_verified);
}

bool VerificationSession::verifyExact32MiB(
    const std::vector<std::uint8_t>& image,
    const BlockVerifiedCallback& block_verified) const
{
    if (image.size() != four_window_size)
        throw std::invalid_argument(
            "exact 32-MiB verification requires a 32-MiB image");

    if (!statusSequence()) return false;
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

    if (!statusSequence()) return false;
    if (!protocol_.tx92Two(0x55, 0xAA, "VERIFY256READ 55AA")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY256READ 0000 A")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY256READ 0000 B")) return false;
    if (!protocol_.tx92Two(0x00, 0x00, "VERIFY256READ 0000 C")) return false;

    return verifyLinear(image, image.size(), block_verified);
}

bool VerificationSession::verifyLinear(
    const std::vector<std::uint8_t>& image,
    std::size_t verify_size,
    const BlockVerifiedCallback& block_verified) const
{
    for (std::size_t offset = 0; offset < verify_size; ) {
        const std::size_t length =
            std::min(block_size, verify_size - offset);
        if (!transport_.out(readCommand(offset, length), usb_timeout_ms))
            return false;

        std::vector<std::uint8_t> received;
        if (!transport_.inExact(received, length, usb_timeout_ms))
            return false;
        if (!compareBlock(image, offset, received)) return false;

        if (block_verified) block_verified(offset, length);
        offset += length;
    }
    return true;
}

} // namespace ezfadvance
