#include "ezfadvance/save_bank_workflow.hpp"

#include "ezfadvance/save_bank_cleaner.hpp"
#include "ezfadvance/save_file_store.hpp"
#include "ezfadvance/save_memory_reader.hpp"
#include "ezfadvance/save_memory_writer.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

namespace ezfadvance {
namespace {

std::string hex16(std::uint16_t value)
{
    std::ostringstream formatted;
    formatted << "0x" << std::hex << std::setw(4) << std::setfill('0')
              << value;
    return formatted.str();
}

std::string backupFailureContext(bool has_backup)
{
    return has_backup ? " The original backup is available.\n"
                      : " No backup was requested.\n";
}

} // namespace

DirectSaveBankWorkflow::DirectSaveBankWorkflow(
    SaveMemoryReader& reader,
    SaveMemoryWriter& writer,
    const SaveFileStore& files,
    SaveBankSelector first_bank,
    std::size_t bank_count,
    std::optional<std::string> output_path,
    std::optional<std::vector<std::uint8_t>> input,
    std::optional<std::string> backup_path,
    std::ostream& output,
    std::ostream& errors)
    : reader_(reader), writer_(writer), files_(files),
      first_bank_(first_bank), bank_count_(bank_count),
      output_path_(std::move(output_path)), input_(std::move(input)),
      backup_path_(std::move(backup_path)), output_(output), errors_(errors)
{
}

int DirectSaveBankWorkflow::run()
{
    const std::size_t size = bank_count_ * SaveBankSelector::bank_size;
    output_ << "Using explicitly requested physical save-bank range; "
               "ROM initialization and catalog allocation were bypassed.\n"
            << "Selected save bank: " << hex16(first_bank_.value())
            << "\n\nReading explicitly selected save-bank range...\n";

    std::vector<std::uint8_t> current;
    if (!reader_.read(size, current, first_bank_.value())) {
        errors_ << "Save read failed.\n";
        return 2;
    }
    return input_ ? write(current, size) : dump(current);
}

int DirectSaveBankWorkflow::write(
    const std::vector<std::uint8_t>& current,
    std::size_t size)
{
    if (input_->size() != size) {
        errors_ << "Internal error: invalid direct save-write request.\n";
        return 1;
    }
    if (backup_path_) {
        if (!files_.writeNew(*backup_path_, current, errors_)) return 5;
        output_ << "Existing save backed up to: " << *backup_path_ << "\n";
    }
    output_ << "Writing " << size << "-byte save across " << bank_count_
            << " bank" << (bank_count_ == 1 ? "" : "s")
            << " beginning at selector " << hex16(first_bank_.value())
            << "...\n";
    if (!writer_.write(first_bank_.value(), *input_)) {
        errors_ << "Save write failed."
                << backupFailureContext(backup_path_.has_value());
        return 2;
    }

    std::vector<std::uint8_t> verification;
    output_ << "Reading save back for byte-for-byte verification...\n";
    if (!reader_.read(size, verification, first_bank_.value())) {
        errors_ << "Save read-back failed."
                << backupFailureContext(backup_path_.has_value());
        return 2;
    }
    if (verification != *input_) {
        const auto mismatch = std::mismatch(
            verification.begin(), verification.end(), input_->begin());
        const auto offset = static_cast<std::size_t>(
            std::distance(verification.begin(), mismatch.first));
        errors_ << "SAVE VERIFICATION FAILED at byte offset 0x"
                << std::hex << offset << std::dec
                << (backup_path_ ? ". The original backup is available.\n"
                                 : ". No backup was requested.\n");
        return 6;
    }

    output_ << "\n========================================\n"
            << "SAVE WRITE AND VERIFICATION COMPLETE\n"
            << "========================================\n"
            << "ROM          : (direct bank access)\n"
            << "Backup       : "
            << (backup_path_ ? *backup_path_ : "(not requested)") << "\n"
            << "Size         : " << size << " bytes (" << size / 1024
            << " KiB)\n"
            << "Write        : " << bank_count_
            << " x 0x92/01 from selector " << hex16(first_bank_.value())
            << ", each with one 0x8000-byte OUT\n"
            << "Verification : full byte-for-byte 0x91/01 read-back matched\n";
    return 0;
}

int DirectSaveBankWorkflow::dump(const std::vector<std::uint8_t>& save)
{
    const std::string path = output_path_.value_or(
        "save-bank-" + hex16(first_bank_.value()).substr(2) + ".sav");
    if (!files_.write(path, save, errors_)) return 5;

    const auto nonzero = std::count_if(
        save.begin(), save.end(), [](std::uint8_t byte) { return byte != 0; });
    const auto nonff = std::count_if(
        save.begin(), save.end(),
        [](std::uint8_t byte) { return byte != 0xff; });
    output_ << "\n========================================\n"
            << "SAVE DUMP COMPLETE\n"
            << "========================================\n"
            << "ROM          : (direct bank access)\n"
            << "Output       : " << path << "\n"
            << "Size         : " << save.size() << " bytes ("
            << save.size() / 1024 << " KiB)\n"
            << "Non-zero     : " << nonzero << " bytes\n"
            << "Non-FF       : " << nonff << " bytes\n"
            << "Protocol     : " << bank_count_
            << " x 0x91/01 from selector " << hex16(first_bank_.value())
            << ", each with one 0x8000-byte IN\n\n"
            << "No save-write payload, ROM-program, or erase operation was "
               "performed by this extraction.\n";
    return 0;
}

SaveBankEraseWorkflow::SaveBankEraseWorkflow(
    SaveMemoryReader& reader,
    SaveBankCleaner& cleaner,
    const SaveFileStore& files,
    std::uint16_t first_selector,
    std::size_t bank_count,
    std::optional<std::string> backup_path,
    std::ostream& output,
    std::ostream& errors)
    : reader_(reader), cleaner_(cleaner), files_(files),
      first_selector_(first_selector), bank_count_(bank_count),
      backup_path_(std::move(backup_path)), output_(output), errors_(errors)
{
}

int SaveBankEraseWorkflow::run()
{
    const std::size_t size = bank_count_ * SaveBankSelector::bank_size;
    if (backup_path_) {
        std::vector<std::uint8_t> current;
        output_ << "Reading selected save range for backup...\n";
        if (!reader_.read(size, current, first_selector_)) {
            errors_ << "Could not read the save range; nothing was erased.\n";
            return 2;
        }
        if (!files_.writeNew(*backup_path_, current, errors_)) return 5;
        output_ << "Existing save backed up to: " << *backup_path_ << "\n";
    }

    output_ << "WARNING: --erase will overwrite " << bank_count_
            << " save bank" << (bank_count_ == 1 ? "" : "s")
            << " with zero bytes, beginning at " << hex16(first_selector_)
            << ".\n";
    if (!cleaner_.clearRange(first_selector_, bank_count_, output_)) {
        errors_ << "Save-bank erase failed.\n";
        return 2;
    }

    std::vector<std::uint8_t> verification;
    output_ << "Reading cleared save range back for verification...\n";
    if (!reader_.read(size, verification, first_selector_) ||
        std::any_of(verification.begin(), verification.end(),
                    [](std::uint8_t value) { return value != 0; })) {
        errors_ << "SAVE-BANK ERASE VERIFICATION FAILED.\n";
        return 6;
    }

    output_ << "\n========================================\n"
            << "SAVE-BANK ERASE COMPLETE\n"
            << "========================================\n"
            << "First bank   : " << hex16(first_selector_) << "\n"
            << "Bank count   : " << bank_count_ << "\n"
            << "Backup       : "
            << (backup_path_ ? *backup_path_ : "(not requested)") << "\n"
            << "Size         : " << size << " bytes\n"
            << "Verification : every byte is zero\n";
    return 0;
}

} // namespace ezfadvance
