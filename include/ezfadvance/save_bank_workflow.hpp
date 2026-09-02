#pragma once

#include "ezfadvance/save_bank_selector.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace ezfadvance {

class SaveBankCleaner;
class SaveFileStore;
class SaveMemoryReader;
class SaveMemoryWriter;

class DirectSaveBankWorkflow final {
public:
    DirectSaveBankWorkflow(SaveMemoryReader& reader,
                           SaveMemoryWriter& writer,
                           const SaveFileStore& files,
                           SaveBankSelector first_bank,
                           std::size_t bank_count,
                           std::optional<std::string> output_path,
                           std::optional<std::vector<std::uint8_t>> input,
                           std::optional<std::string> backup_path,
                           std::ostream& output,
                           std::ostream& errors);

    int run();

private:
    int write(const std::vector<std::uint8_t>& current, std::size_t size);
    int dump(const std::vector<std::uint8_t>& save);

    SaveMemoryReader& reader_;
    SaveMemoryWriter& writer_;
    const SaveFileStore& files_;
    SaveBankSelector first_bank_;
    std::size_t bank_count_;
    std::optional<std::string> output_path_;
    std::optional<std::vector<std::uint8_t>> input_;
    std::optional<std::string> backup_path_;
    std::ostream& output_;
    std::ostream& errors_;
};

class SaveBankEraseWorkflow final {
public:
    SaveBankEraseWorkflow(SaveMemoryReader& reader,
                          SaveBankCleaner& cleaner,
                          const SaveFileStore& files,
                          std::uint16_t first_selector,
                          std::size_t bank_count,
                          std::optional<std::string> backup_path,
                          std::ostream& output,
                          std::ostream& errors);

    int run();

private:
    SaveMemoryReader& reader_;
    SaveBankCleaner& cleaner_;
    const SaveFileStore& files_;
    std::uint16_t first_selector_;
    std::size_t bank_count_;
    std::optional<std::string> backup_path_;
    std::ostream& output_;
    std::ostream& errors_;
};

} // namespace ezfadvance
