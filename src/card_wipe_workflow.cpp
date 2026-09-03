#include "ezfadvance/card_wipe_workflow.hpp"

#include <algorithm>
#include <iomanip>
#include <iterator>
#include <ostream>
#include <thread>
#include <utility>

#include "ezfadvance/protocol.hpp"

namespace ezfadvance {
namespace {

constexpr unsigned timeout_ms = 5000;
constexpr unsigned readiness_attempts = 5;
constexpr auto readiness_retry_delay = std::chrono::milliseconds(100);

std::vector<std::uint32_t> bottomBootSectorAddresses()
{
    std::vector<std::uint32_t> addresses;
    for (std::uint32_t address = 0; address <= 0x008000; address += 0x001000)
        addresses.push_back(address);
    for (std::uint32_t address = 0x010000;
         address <= 0x3F8000; address += 0x008000)
        addresses.push_back(address);
    return addresses;
}

std::vector<std::uint32_t> topBootSectorAddresses()
{
    std::vector<std::uint32_t> addresses;
    for (std::uint32_t address = 0; address <= 0x3F8000; address += 0x008000)
        addresses.push_back(address);
    for (std::uint32_t address = 0x3F9000;
         address <= 0x3FF000; address += 0x001000)
        addresses.push_back(address);
    return addresses;
}

bool allErased(const std::vector<std::uint8_t>& bytes)
{
    return std::all_of(bytes.begin(), bytes.end(),
                       [](std::uint8_t byte) { return byte == 0xFF; });
}

void printHex(std::ostream& output,
              const std::uint8_t* bytes,
              std::size_t size)
{
    for (std::size_t index = 0; index < size; ++index) {
        if (index && index % 16 == 0) output << '\n';
        output << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<unsigned>(bytes[index]) << ' ';
    }
    output << std::dec << '\n';
}

} // namespace

CardWipeWorkflow::CardWipeWorkflow(Transport& transport,
                                   FlashWindowSelector& flash_windows,
                                   SaveBankCleaner& save_bank_cleaner,
                                   std::ostream& output,
                                   std::ostream& error,
                                   Sleep sleep)
    : transport_(transport),
      flash_windows_(flash_windows),
      save_bank_cleaner_(save_bank_cleaner),
      output_(output),
      error_(error),
      sleep_(std::move(sleep))
{
    if (!sleep_) {
        sleep_ = [](std::chrono::milliseconds duration) {
            std::this_thread::sleep_for(duration);
        };
    }
}

bool CardWipeWorkflow::cartridgeReadyPreflight()
{
    const std::vector<std::uint8_t> reset =
        {0x5A,0xA5,0x97,0,0,0,0,0,0,0,0,0,0};
    const std::vector<std::uint8_t> ready =
        {0x5A,0xA5,0x98,0,0,0,0,0,0,0,0,0,0};
    const std::vector<std::uint8_t> setup =
        {0x5A,0xA5,0x99,0,1,0,0,0,0,0,0,0,0};
    std::vector<std::uint8_t> response;

    output_ << "\n========================================\n"
            << "CARTRIDGE PREFLIGHT\n"
            << "========================================\n";

    if (!transport_.out(reset, timeout_ms) ||
        !transport_.inMax(response, 1, timeout_ms) ||
        response.size() != 1 || response[0] != 0) {
        error_ << "CARTRIDGE PREFLIGHT FAILED: 0x97 bridge reset did not "
                  "return 00.\nNo erase operation was attempted.\n";
        return false;
    }
    output_ << "0x97 -> 00 (bridge reset)\n";

    bool cartridge_ready = false;
    for (unsigned attempt = 1; attempt <= readiness_attempts; ++attempt) {
        if (!transport_.out(ready, timeout_ms)) {
            error_ << "CARTRIDGE PREFLIGHT FAILED: could not send the 0x98 "
                      "readiness command.\n"
                   << "Check the EZF Advance III USB connection and try again.\n"
                   << "No erase operation was attempted.\n";
            return false;
        }
        if (!transport_.inMax(response, 1, timeout_ms) || response.size() != 1) {
            error_ << "GBA CARTRIDGE NOT DETECTED / NOT READY.\n"
                   << "The EZF Advance III did not return the required 0x98 "
                      "readiness byte (expected 01).\n"
                   << "No erase operation was attempted.\n";
            return false;
        }
        if (response[0] == 1) {
            cartridge_ready = true;
            break;
        }
        if (response[0] != 0) {
            error_ << "Unexpected 0x98 readiness value 0x"
                   << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<unsigned>(response[0])
                   << std::dec << std::setfill(' ') << ".\n"
                   << "No erase operation was attempted.\n";
            return false;
        }
        if (attempt != readiness_attempts) {
            output_ << "0x98 readiness returned 00; retrying ("
                    << attempt << '/' << readiness_attempts << ")...\n";
            sleep_(readiness_retry_delay);
        }
    }
    if (!cartridge_ready) {
        error_ << "GBA CARTRIDGE NOT DETECTED / NOT READY.\n"
               << "0x98 readiness remained 0x00 after " << readiness_attempts
               << " checks.\n"
               << "Make sure an EZ-Flash Advance III cartridge is fully "
                  "inserted, then retry.\n"
               << "No erase operation was attempted.\n";
        return false;
    }
    output_ << "0x98 -> 01 (cartridge inserted / ready)\n";

    if (!transport_.out(setup, timeout_ms) ||
        !transport_.inMax(response, setup.size(), timeout_ms) ||
        response != setup) {
        error_ << "CARTRIDGE PREFLIGHT FAILED: 0x99 bridge setup echo "
                  "did not match.\nNo erase operation was attempted.\n";
        return false;
    }
    output_ << "0x99 parameter 01 echo OK (bridge configured)\n";
    return true;
}

bool CardWipeWorkflow::tx92Two(std::uint8_t first,
                               std::uint8_t second,
                               const char* label)
{
    return Protocol(transport_).commandDataEcho(
        Protocol::command92Two(), {first, second}, label,
        {timeout_ms, 0, true});
}

bool CardWipeWorkflow::finalCleanup()
{
    return tx92Two(0x55, 0xAA, "CLEANUP 55AA") &&
           tx92Two(0, 0, "CLEANUP 0000 A") &&
           tx92Two(0, 0, "CLEANUP 0000 B") &&
           tx92Two(0, 0, "CLEANUP 0000 C");
}

bool CardWipeWorkflow::eraseSector(std::uint32_t address,
                                   unsigned bank,
                                   std::size_t sector_index,
                                   std::size_t sector_count)
{
    const std::vector<std::uint8_t> command = {
        0x5A,0xA5,0x96,0,
        static_cast<std::uint8_t>(address),
        static_cast<std::uint8_t>(address >> 8),
        static_cast<std::uint8_t>(address >> 16),
        static_cast<std::uint8_t>(address >> 24),
        0,0,0,0,0};
    if (!transport_.out(command, timeout_ms)) {
        error_ << "Erase command failed at bank " << bank
               << " address 0x" << std::hex << address << std::dec << '\n';
        return false;
    }

    std::vector<std::uint8_t> response;
    if (!transport_.inMax(response, 64, timeout_ms)) {
        error_ << "Erase response failed at bank " << bank
               << " address 0x" << std::hex << address << std::dec << '\n';
        return false;
    }
    if (response.size() != 13 ||
        !std::equal(command.begin(), command.begin() + 12, response.begin())) {
        error_ << "Unexpected 0x96 response at bank " << bank
               << " address 0x" << std::hex << address << std::dec
               << "\nExpected prefix:\n";
        printHex(output_, command.data(), 12);
        error_ << "Received:\n";
        printHex(output_, response.data(), response.size());
        return false;
    }
    if (response[12] != 0) {
        error_ << "0x96 erase status is non-zero at bank " << bank
               << " address 0x" << std::hex << address
               << ": status=0x" << static_cast<unsigned>(response[12])
               << std::dec << '\n';
        return false;
    }

    if (sector_index == 0 || ((sector_index + 1) % 8) == 0 ||
        sector_index + 1 == sector_count) {
        output_ << "  bank " << (bank + 1) << "/4: sector "
                << (sector_index + 1) << '/' << sector_count
                << " @ 0x" << std::hex << std::setw(6) << std::setfill('0')
                << address << std::dec << '\n';
    }
    return true;
}

bool CardWipeWorkflow::eraseBank(unsigned bank)
{
    const auto sectors = (bank == 0 || bank == 2)
        ? bottomBootSectorAddresses() : topBootSectorAddresses();
    if (sectors.size() != 135) {
        error_ << "Internal error: expected 135 sectors, got "
               << sectors.size() << '\n';
        return false;
    }

    output_ << "\n========================================\n"
            << "ERASING FLASH BANK " << (bank + 1) << "/4\n"
            << "========================================\n";
    if (!flash_windows_.select(bank)) {
        error_ << "Bank setup failed for bank " << bank << '\n';
        return false;
    }
    for (std::size_t index = 0; index < sectors.size(); ++index) {
        if (!eraseSector(sectors[index], bank, index, sectors.size()))
            return false;
    }
    if (!flash_windows_.finishOperation()) {
        error_ << "Post-erase status sequence failed for bank " << bank << '\n';
        return false;
    }
    return true;
}

bool CardWipeWorkflow::readRegion(
    const std::vector<std::uint8_t>& command,
    std::size_t expected_length,
    std::vector<std::uint8_t>& result)
{
    if (!transport_.out(command, timeout_ms) ||
        !transport_.inMax(result, expected_length, 10000)) return false;
    if (result.size() != expected_length) {
        error_ << "Blank verification read returned " << result.size()
               << " bytes, expected " << expected_length << '\n';
        return false;
    }
    return true;
}

bool CardWipeWorkflow::verifyBlankLikeCapture()
{
    output_ << "\n========================================\n"
            << "CAPTURE-DERIVED BLANK VERIFICATION\n"
            << "========================================\n";
    const std::vector<std::uint8_t> first_command = {
        0x5A,0xA5,0x91,0, 0,0,0,0, 0xAC,0,0,0,0};
    const std::vector<std::uint8_t> second_command = {
        0x5A,0xA5,0x91,0, 2,0,0,2, 0x20,0,0,0,0};
    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;
    if (!readRegion(first_command, 172, first)) return false;
    if (!allErased(first)) {
        const auto mismatch = std::find_if(first.begin(), first.end(),
            [](std::uint8_t byte) { return byte != 0xFF; });
        error_ << "Blank verification #1 failed at offset 0x" << std::hex
               << std::distance(first.begin(), mismatch) << std::dec
               << ": 0x" << std::hex << static_cast<unsigned>(*mismatch)
               << std::dec << '\n';
        return false;
    }
    output_ << "Verification #1: 172/172 bytes are FF\n";

    if (!readRegion(second_command, 32, second)) return false;
    if (!allErased(second)) {
        const auto mismatch = std::find_if(second.begin(), second.end(),
            [](std::uint8_t byte) { return byte != 0xFF; });
        error_ << "Blank verification #2 failed at offset 0x" << std::hex
               << std::distance(second.begin(), mismatch) << std::dec
               << ": 0x" << std::hex << static_cast<unsigned>(*mismatch)
               << std::dec << '\n';
        return false;
    }
    output_ << "Verification #2: 32/32 bytes are FF\n";
    return true;
}

bool CardWipeWorkflow::execute()
{
    bool ok = cartridgeReadyPreflight();
    for (unsigned bank = 0; bank < 4 && ok; ++bank)
        ok = eraseBank(bank);

    if (ok) {
        output_ << "\n========================================\n"
                << "CLEARING SAVE MEMORY\n"
                << "========================================\n";
        ok = save_bank_cleaner_.clearAll(output_);
        if (ok) output_ << "All four save banks explicitly cleared to zero.\n";
    }
    if (ok) {
        output_ << "\nRunning final cleanup sequence...\n";
        ok = finalCleanup();
    }
    if (ok) ok = verifyBlankLikeCapture();
    return ok;
}

} // namespace ezfadvance
