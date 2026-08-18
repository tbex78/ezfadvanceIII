#include "ezfadvance/cartridge_format.hpp"

#include <stdexcept>

namespace ezfadvance {

std::uint16_t CartridgeFormat::readLe16(const std::uint8_t* bytes) noexcept
{
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(bytes[1] << 8);
}

std::uint32_t CartridgeFormat::readLe32(const std::uint8_t* bytes) noexcept
{
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::string CartridgeFormat::cleanAscii(const std::uint8_t* bytes,
                                        std::size_t size)
{
    std::string result;
    for (std::size_t i = 0; i < size; ++i) {
        const std::uint8_t value = bytes[i];
        if (value == 0x00 || value == 0xFF)
            break;
        result.push_back(value >= 0x20 && value <= 0x7E
            ? static_cast<char>(value) : '.');
    }
    while (!result.empty() && result.back() == ' ')
        result.pop_back();
    return result;
}

bool CartridgeFormat::gbaHeaderChecksumValid(
    const std::vector<std::uint8_t>& bytes) noexcept
{
    if (bytes.size() < 0xC0)
        return false;
    std::uint8_t checksum = 0;
    for (std::size_t i = 0xA0; i <= 0xBC; ++i)
        checksum = static_cast<std::uint8_t>(checksum - bytes[i]);
    checksum = static_cast<std::uint8_t>(checksum - 0x19);
    return checksum == bytes[0xBD];
}

std::optional<std::uint32_t> CartridgeFormat::armBranchTarget(
    std::uint32_t instruction) noexcept
{
    if ((instruction & 0xFF000000u) != 0xEA000000u)
        return std::nullopt;

    std::int32_t immediate = static_cast<std::int32_t>(instruction & 0x00FFFFFFu);
    if (immediate & 0x00800000)
        immediate |= static_cast<std::int32_t>(0xFF000000u);

    const std::int64_t target = 8ll + static_cast<std::int64_t>(immediate) * 4ll;
    if (target < 0 || target > 0xFFFFFFFFll)
        return std::nullopt;
    return static_cast<std::uint32_t>(target);
}

std::uint32_t CartridgeFormat::makeArmBranch(std::uint32_t target)
{
    if (target < 8 || ((target - 8u) & 3u))
        throw std::invalid_argument("target cannot be encoded as ARM B");

    const std::int64_t immediate =
        (static_cast<std::int64_t>(target) - 8ll) / 4ll;
    if (immediate < -(1ll << 23) || immediate >= (1ll << 23))
        throw std::out_of_range("target is outside ARM B range");

    return 0xEA000000u |
           (static_cast<std::uint32_t>(immediate) & 0x00FFFFFFu);
}

GbaHeader GbaHeader::parse(const std::vector<std::uint8_t>& bytes)
{
    GbaHeader header;
    if (bytes.size() < 0xC0)
        return header;
    header.readable = true;
    header.title = CartridgeFormat::cleanAscii(bytes.data() + 0xA0, 12);
    header.game_code = CartridgeFormat::cleanAscii(bytes.data() + 0xAC, 4);
    header.maker_code = CartridgeFormat::cleanAscii(bytes.data() + 0xB0, 2);
    header.version = bytes[0xBC];
    header.checksum_ok = CartridgeFormat::gbaHeaderChecksumValid(bytes);
    return header;
}

CatalogEntry CatalogEntry::parse(const std::vector<std::uint8_t>& loader,
                                 std::size_t offset,
                                 bool first)
{
    constexpr std::size_t entry_size = 28;
    if (offset + entry_size > loader.size())
        throw std::runtime_error("catalog entry outside read loader data");

    CatalogEntry entry;
    entry.name = CartridgeFormat::cleanAscii(loader.data() + offset, 16);
    const auto type_field = CartridgeFormat::readLe32(loader.data() + offset + 16);
    entry.type = static_cast<std::uint8_t>(type_field >> 24);
    entry.packed_start = CartridgeFormat::readLe32(loader.data() + offset + 20);
    entry.mapping = static_cast<std::uint8_t>(entry.packed_start & 0xFFu);
    entry.start = first ? 0u : static_cast<std::uint32_t>((entry.packed_start >> 8) * 2u);
    entry.target_or_start = CartridgeFormat::readLe32(loader.data() + offset + 24);
    return entry;
}

bool CatalogEntry::plausible(std::uint32_t image_limit, bool first) const noexcept
{
    if (name.empty() || start >= image_limit)
        return false;
    return first || (start & 0xFFFFu) == 0;
}

} // namespace ezfadvance
