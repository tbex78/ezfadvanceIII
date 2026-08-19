#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ezfadvance {

struct GbaHeader {
    bool readable = false;
    bool checksum_ok = false;
    std::string title;
    std::string game_code;
    std::string maker_code;
    std::uint8_t version = 0;

    static GbaHeader parse(const std::vector<std::uint8_t>& bytes);
};

struct CatalogEntry {
    std::string name;
    std::uint8_t type = 0;
    std::uint8_t mapping = 0;
    std::uint32_t packed_start = 0;
    std::uint32_t start = 0;
    std::uint32_t target_or_start = 0;

    static CatalogEntry parse(const std::vector<std::uint8_t>& loader,
                              std::size_t offset,
                              bool first);

    bool plausible(std::uint32_t image_limit, bool first) const noexcept;
};

class CartridgeFormat final {
public:
    static std::uint16_t readLe16(const std::uint8_t* bytes) noexcept;
    static std::uint32_t readLe32(const std::uint8_t* bytes) noexcept;
    static std::string cleanAscii(const std::uint8_t* bytes, std::size_t size);
    static bool gbaHeaderChecksumValid(
        const std::vector<std::uint8_t>& bytes) noexcept;
    static bool validGbaRomHeader(
        const std::vector<std::uint8_t>& bytes) noexcept;
    static std::optional<std::uint32_t> armBranchTarget(
        std::uint32_t instruction) noexcept;
    static std::uint32_t makeArmBranch(std::uint32_t target);
};

} // namespace ezfadvance
