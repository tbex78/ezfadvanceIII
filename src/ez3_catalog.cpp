#include "ezfadvance/ez3_catalog.hpp"

namespace ezfadvance {

std::optional<std::uint32_t> Ez3CatalogLayout::allocationEnd(
    std::size_t index,
    std::uint32_t loader_start,
    std::uint32_t image_limit) const noexcept
{
    if (index >= entries.size())
        return std::nullopt;
    const CatalogEntry& entry = entries[index];
    const auto stored_end = entry.storedEnd(image_limit);
    if (!stored_end)
        return std::nullopt;

    std::uint32_t end = *stored_end + 1;
    if (index + 1 < entries.size()) {
        if (entries[index + 1].start <= entry.start)
            return std::nullopt;
        if (entries[index + 1].start < end)
            end = entries[index + 1].start;
    } else if (loader_start > entry.start && loader_start < end) {
        // An appended loader terminates the last allocation. A loader before
        // the last ROM is embedded in an earlier erased region and must not
        // collapse the last ROM's span to zero.
        end = loader_start;
    }
    return end > entry.start ? std::optional<std::uint32_t>(end)
                             : std::nullopt;
}

namespace {

constexpr std::size_t single_header_offset = 0x4E8;
constexpr std::size_t single_header_count2_offset = 0x4F6;
constexpr std::size_t single_entry_offset = 0x4F8;
constexpr std::size_t multi_header_offset = 0x475E;
constexpr std::size_t multi_entry_offset = 0x476E;
constexpr std::size_t multi_catalog_end_offset = 0x548E;

static_assert(multi_entry_offset +
              Ez3CatalogParser::structural_slot_count *
                  Ez3CatalogParser::entry_size ==
              multi_catalog_end_offset,
              "multi-ROM catalog must contain 120 whole entries");

std::optional<Ez3CatalogLayout> parseEntries(
    Ez3CatalogKind kind,
    const std::vector<std::uint8_t>& loader,
    std::size_t entry_offset,
    std::size_t count,
    std::uint32_t image_limit)
{
    if (count == 0 || count > Ez3CatalogParser::structural_slot_count ||
        entry_offset > loader.size() ||
        count > (loader.size() - entry_offset) /
                    Ez3CatalogParser::entry_size)
        return std::nullopt;

    Ez3CatalogLayout layout;
    layout.kind = kind;
    layout.entries.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const bool first = index == 0;
        const CatalogEntry entry = CatalogEntry::parse(
            loader, entry_offset + index * Ez3CatalogParser::entry_size,
            first);
        if (!entry.plausible(image_limit, first))
            return std::nullopt;
        layout.entries.push_back(entry);
    }
    return layout;
}

} // namespace

std::optional<Ez3CatalogLayout> Ez3CatalogParser::parse(
    const std::vector<std::uint8_t>& loader,
    std::uint32_t image_limit)
{
    if (image_limit == 0)
        return std::nullopt;

    if (loader.size() >= single_entry_offset + entry_size &&
        CartridgeFormat::readLe16(loader.data() + single_header_offset) == 1 &&
        CartridgeFormat::readLe16(
            loader.data() + single_header_count2_offset) == 1) {
        if (const auto layout = parseEntries(
                Ez3CatalogKind::single, loader, single_entry_offset, 1,
                image_limit))
            return layout;
    }

    if (loader.size() < multi_header_offset + 16)
        return std::nullopt;

    const std::uint16_t count =
        CartridgeFormat::readLe16(loader.data() + multi_header_offset);
    const std::uint16_t repeated_count =
        CartridgeFormat::readLe16(loader.data() + multi_header_offset + 14);
    if (count < 2 || count > structural_slot_count || count != repeated_count)
        return std::nullopt;

    return parseEntries(Ez3CatalogKind::multi, loader, multi_entry_offset,
                        count, image_limit);
}

} // namespace ezfadvance
