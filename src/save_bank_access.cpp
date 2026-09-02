#include "ezfadvance/save_bank_access.hpp"

namespace ezfadvance {

bool SaveBankAccess::select(std::uint16_t bank_value,
                            const std::string& label_prefix) const
{
    const auto low = static_cast<std::uint8_t>(bank_value & 0xffu);
    const auto high = static_cast<std::uint8_t>(bank_value >> 8);
    return protocol_.tx92Two(0x55, 0xaa, label_prefix + " 55AA") &&
           protocol_.tx92Two(0, 0, label_prefix + " 0000 A") &&
           protocol_.tx92Two(0, 0, label_prefix + " 0000 B") &&
           protocol_.tx92Two(low, high, label_prefix + " selector") &&
           protocol_.tx92Two(0, 0, label_prefix + " 0000 C") &&
           protocol_.tx92Two(0, 0, label_prefix + " 0000 D") &&
           protocol_.tx92Two(0, 0, label_prefix + " 0000 E") &&
           protocol_.tx92Two(0, 0, label_prefix + " 0000 F");
}

} // namespace ezfadvance
