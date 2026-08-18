#pragma once

#include "ezfadvance/usb_device.hpp"

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ezfadvance::test {

class TranscriptTransport final : public Transport {
public:
    using Transport::out;

    enum class Direction {
        out,
        in_exact,
        in_max
    };

    struct ExpectedTransfer {
        Direction direction;
        std::vector<std::uint8_t> bytes;
        std::size_t requested_size;
        unsigned timeout_ms;
        bool succeeds;
    };

    TranscriptTransport& expectOut(std::vector<std::uint8_t> bytes,
                                   unsigned timeout_ms,
                                   bool succeeds = true)
    {
        expected_.push_back({Direction::out, std::move(bytes), 0,
                             timeout_ms, succeeds});
        return *this;
    }

    TranscriptTransport& expectInExact(std::vector<std::uint8_t> response,
                                       std::size_t wanted,
                                       unsigned timeout_ms,
                                       bool succeeds = true)
    {
        expected_.push_back({Direction::in_exact, std::move(response), wanted,
                             timeout_ms, succeeds});
        return *this;
    }

    TranscriptTransport& expectInMax(std::vector<std::uint8_t> response,
                                     std::size_t maximum,
                                     unsigned timeout_ms,
                                     bool succeeds = true)
    {
        expected_.push_back({Direction::in_max, std::move(response), maximum,
                             timeout_ms, succeeds});
        return *this;
    }

    bool out(const std::uint8_t* data, std::size_t size,
             unsigned timeout_ms) const override
    {
        observed_out_.emplace_back(data, data + size);
        const ExpectedTransfer* transfer = next(Direction::out, timeout_ms);
        if (!transfer) return false;
        const std::vector<std::uint8_t> actual(data, data + size);
        if (actual != transfer->bytes) {
            const std::size_t mismatch =
                firstMismatch(transfer->bytes, actual);
            fail("transfer " + std::to_string(cursor_) +
                 " OUT mismatch\nfirst differing byte: " +
                 std::to_string(mismatch) + "\nexpected (" +
                 std::to_string(transfer->bytes.size()) + " bytes): " +
                 hex(transfer->bytes) + "\nreceived (" +
                 std::to_string(actual.size()) + " bytes): " + hex(actual));
            return false;
        }
        return transfer->succeeds;
    }

    bool inExact(std::vector<std::uint8_t>& data, std::size_t wanted,
                 unsigned timeout_ms) const override
    {
        return receive(data, Direction::in_exact, wanted, timeout_ms);
    }

    bool inMax(std::vector<std::uint8_t>& data, std::size_t maximum,
               unsigned timeout_ms) const override
    {
        return receive(data, Direction::in_max, maximum, timeout_ms);
    }

    bool complete() const
    {
        if (error_.empty() && cursor_ != expected_.size()) {
            fail("transcript incomplete: " + std::to_string(remaining()) +
                 " expected transfer(s) not observed");
        }
        return error_.empty();
    }

    std::size_t remaining() const noexcept
    {
        return expected_.size() - cursor_;
    }

    const std::string& error() const noexcept { return error_; }
    const std::vector<std::vector<std::uint8_t>>& observedOut() const noexcept
    {
        return observed_out_;
    }

private:
    const ExpectedTransfer* next(Direction actual_direction,
                                 unsigned actual_timeout) const
    {
        if (!error_.empty()) return nullptr;
        if (cursor_ == expected_.size()) {
            fail("transfer " + std::to_string(cursor_ + 1) +
                 ": unexpected " + directionName(actual_direction) +
                 " transfer after transcript end");
            return nullptr;
        }

        const ExpectedTransfer& transfer = expected_[cursor_++];
        if (transfer.direction != actual_direction) {
            fail("transfer " + std::to_string(cursor_) + ": expected " +
                 directionName(transfer.direction) + ", received " +
                 directionName(actual_direction));
            return nullptr;
        }
        if (transfer.timeout_ms != actual_timeout) {
            fail("transfer " + std::to_string(cursor_) +
                 ": expected timeout " +
                 std::to_string(transfer.timeout_ms) + ", received " +
                 std::to_string(actual_timeout));
            return nullptr;
        }
        return &transfer;
    }

    bool receive(std::vector<std::uint8_t>& data, Direction direction,
                 std::size_t requested_size, unsigned timeout_ms) const
    {
        const ExpectedTransfer* transfer = next(direction, timeout_ms);
        if (!transfer) return false;
        if (transfer->requested_size != requested_size) {
            fail("transfer " + std::to_string(cursor_) +
                 ": expected requested size " +
                 std::to_string(transfer->requested_size) + ", received " +
                 std::to_string(requested_size));
            return false;
        }
        if (!transfer->succeeds) {
            data.clear();
            return false;
        }
        data = transfer->bytes;
        if (direction == Direction::in_exact && data.size() != requested_size) {
            fail("transfer " + std::to_string(cursor_) +
                 " IN exact fixture response size mismatch: requested " +
                 std::to_string(requested_size) + ", fixture returns " +
                 std::to_string(data.size()));
            return false;
        }
        if (direction == Direction::in_max && data.size() > requested_size) {
            fail("transfer " + std::to_string(cursor_) +
                 " IN max fixture response exceeds requested size: maximum " +
                 std::to_string(requested_size) + ", fixture returns " +
                 std::to_string(data.size()));
            return false;
        }
        return true;
    }

    void fail(std::string message) const
    {
        if (error_.empty()) error_ = std::move(message);
    }

    static std::string directionName(Direction direction)
    {
        switch (direction) {
        case Direction::out: return "OUT";
        case Direction::in_exact: return "IN exact";
        case Direction::in_max: return "IN max";
        }
        return "unknown";
    }

    static std::string hex(const std::vector<std::uint8_t>& bytes)
    {
        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            if (i) output << ' ';
            output << std::setw(2) << static_cast<unsigned>(bytes[i]);
        }
        return output.str();
    }

    static std::size_t firstMismatch(
        const std::vector<std::uint8_t>& expected,
        const std::vector<std::uint8_t>& actual) noexcept
    {
        const std::size_t common_size =
            expected.size() < actual.size() ? expected.size() : actual.size();
        for (std::size_t i = 0; i < common_size; ++i) {
            if (expected[i] != actual[i]) return i;
        }
        return common_size;
    }

    std::vector<ExpectedTransfer> expected_;
    mutable std::size_t cursor_ = 0;
    mutable std::string error_;
    mutable std::vector<std::vector<std::uint8_t>> observed_out_;
};

} // namespace ezfadvance::test
