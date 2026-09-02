#include "ezfadvance/ez3_catalog_reader.hpp"

#include <algorithm>
#include <vector>

namespace ezfadvance {

Ez3CatalogReader::Ez3CatalogReader(ReadOnlyCartridge& cartridge,
                                   std::uint32_t image_limit,
                                   std::size_t entry_limit) noexcept
    : cartridge_(cartridge), image_limit_(image_limit),
      entry_limit_(entry_limit)
{
}

Ez3CatalogReadResult Ez3CatalogReader::read()
{
    Ez3CatalogReadResult result;
    const auto branch = cartridge_.readArmBranch();
    if (!branch) return result;
    result.first_bytes = branch->bytes;
    if (!branch->target) {
        result.status = Ez3CatalogReadStatus::missing_branch;
        return result;
    }

    result.loader_start = *branch->target;
    if (result.loader_start < 0xC0 || result.loader_start >= image_limit_) {
        result.status = Ez3CatalogReadStatus::branch_out_of_range;
        return result;
    }

    const auto read_length = std::min<std::size_t>(
        Ez3CatalogParser::loader_read_size,
        static_cast<std::size_t>(image_limit_ - result.loader_start));
    result.loader_end = result.loader_start +
        static_cast<std::uint32_t>(read_length - 1);
    if (!cartridge_.prepareLinearForAddress(result.loader_end))
        return result;

    std::vector<std::uint8_t> loader;
    if (!cartridge_.read(result.loader_start, loader, read_length))
        return result;
    result.catalog = Ez3CatalogParser::parse(loader, image_limit_);
    if (!result.catalog) {
        result.status = Ez3CatalogReadStatus::invalid_catalog;
        return result;
    }
    if (result.catalog->entries.size() > entry_limit_) {
        result.catalog.reset();
        result.status = Ez3CatalogReadStatus::entry_limit_exceeded;
        return result;
    }
    result.status = Ez3CatalogReadStatus::loaded;
    return result;
}

} // namespace ezfadvance
