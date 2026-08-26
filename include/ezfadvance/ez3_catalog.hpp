#pragma once

#include "ezfadvance/cartridge_format.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ezfadvance {

enum class Ez3CatalogKind { single, multi };

struct Ez3CatalogLayout {
    Ez3CatalogKind kind = Ez3CatalogKind::single;
    std::vector<CatalogEntry> entries;

    bool isSingle() const noexcept { return kind == Ez3CatalogKind::single; }
};

class Ez3CatalogParser final {
public:
    static constexpr std::size_t entry_size = 28;
    static constexpr std::size_t structural_slot_count = 120;
    static constexpr std::size_t loader_read_size = 0x7080;

    // Interpret only the capture-derived loader/catalog structure. The caller
    // chooses image_limit according to that application's evidence boundary.
    static std::optional<Ez3CatalogLayout> parse(
        const std::vector<std::uint8_t>& loader,
        std::uint32_t image_limit);
};

} // namespace ezfadvance
