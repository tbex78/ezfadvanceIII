#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ezfadvance {

// A capture-proven EZ3 32-KiB save-memory selector. This value object keeps
// command-line parsing and the four-bank geometry out of application code.
class SaveBankSelector final {
public:
    static std::optional<SaveBankSelector> parse(std::string_view text) noexcept;

    static constexpr std::uint16_t first = 0x0900;
    static constexpr std::uint16_t last = 0x0930;
    static constexpr std::uint16_t stride = 0x0010;
    static constexpr std::size_t bank_size = 0x8000;

    constexpr std::uint16_t value() const noexcept { return value_; }

    constexpr bool accommodates(std::size_t save_size) const noexcept
    {
        if (save_size == 0 || save_size % bank_size != 0) return false;
        const auto banks = save_size / bank_size;
        const auto final = static_cast<std::uint32_t>(value_) +
            static_cast<std::uint32_t>((banks - 1) * stride);
        return final <= last;
    }

private:
    explicit constexpr SaveBankSelector(std::uint16_t value) noexcept
        : value_(value) {}

    std::uint16_t value_;
};

} // namespace ezfadvance
