#pragma once

#include "ezfadvance/ez3_catalog.hpp"
#include "ezfadvance/read_only_cartridge.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ezfadvance {

enum class Ez3CatalogReadStatus {
    loaded,
    read_failed,
    missing_branch,
    branch_out_of_range,
    invalid_catalog,
    entry_limit_exceeded
};

struct Ez3CatalogReadResult {
    Ez3CatalogReadStatus status = Ez3CatalogReadStatus::read_failed;
    std::array<std::uint8_t, 4> first_bytes{};
    std::uint32_t loader_start = 0;
    std::uint32_t loader_end = 0;
    std::optional<Ez3CatalogLayout> catalog;

    explicit operator bool() const noexcept
    {
        return status == Ez3CatalogReadStatus::loaded && catalog.has_value();
    }
};

// Shared, read-only EZ3 loader/catalog discovery for applications that have
// already initialized and positively classified the cartridge as EZ3 flash.
class Ez3CatalogReader final {
public:
    Ez3CatalogReader(ReadOnlyCartridge& cartridge,
                     std::uint32_t image_limit,
                     std::size_t entry_limit) noexcept;

    Ez3CatalogReadResult read();

private:
    ReadOnlyCartridge& cartridge_;
    std::uint32_t image_limit_;
    std::size_t entry_limit_;
};

} // namespace ezfadvance
