#include "ezfadvance/save_bank_selector.hpp"

#include <charconv>
#include <limits>

namespace ezfadvance {

std::optional<SaveBankSelector> SaveBankSelector::parse(
    std::string_view text) noexcept
{
    int base = 10;
    if (text.size() > 2 && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
        base = 16;
    }
    if (text.empty()) return std::nullopt;

    unsigned long value = 0;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), value, base);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        value > std::numeric_limits<std::uint16_t>::max())
        return std::nullopt;

    const auto selector = static_cast<std::uint16_t>(value);
    if (selector < first || selector > last ||
        (selector - first) % stride != 0)
        return std::nullopt;
    return SaveBankSelector(selector);
}

} // namespace ezfadvance
