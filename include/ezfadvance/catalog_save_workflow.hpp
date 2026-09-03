#pragma once

#include "ezfadvance/save_bank_selector.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace ezfadvance {

class ReadOnlyCartridge;
class SaveFileStore;
class SaveMemoryReader;
class SaveMemoryWriter;

struct CatalogSaveRequest {
    std::optional<std::string> output_path;
    std::optional<std::size_t> rom_number;
    std::optional<SaveBankSelector> save_bank;
    std::optional<std::size_t> consecutive_bank_count;
    std::optional<std::vector<std::uint8_t>> input;
    std::optional<std::string> backup_path;
    bool show_catalog = false;
};

// Coordinates catalog discovery, metadata analysis, save-bank planning, and
// catalog-selected save I/O. Physical direct-bank and erase workflows remain
// separate use cases.
class CatalogSaveWorkflow final {
public:
    CatalogSaveWorkflow(ReadOnlyCartridge& cartridge,
                        SaveMemoryReader& reader,
                        SaveMemoryWriter& writer,
                        const SaveFileStore& files,
                        CatalogSaveRequest request,
                        std::ostream& output,
                        std::ostream& errors);

    int run();

private:
    ReadOnlyCartridge& cartridge_;
    SaveMemoryReader& reader_;
    SaveMemoryWriter& writer_;
    const SaveFileStore& files_;
    CatalogSaveRequest request_;
    std::ostream& output_;
    std::ostream& errors_;
};

} // namespace ezfadvance
