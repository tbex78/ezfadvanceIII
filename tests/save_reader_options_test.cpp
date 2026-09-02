#include "ezfadvance/save_reader_options.hpp"

#include <cassert>
#include <sstream>
#include <string>
#include <vector>

namespace {

ezfadvance::SaveReaderOptions parse(std::vector<std::string> arguments)
{
    ezfadvance::SaveReaderOptions options;
    std::ostringstream errors;
    assert(ezfadvance::parseSaveReaderOptions(arguments, options, errors) ==
           ezfadvance::SaveReaderParseStatus::run);
    assert(errors.str().empty());
    return options;
}

void reject(std::vector<std::string> arguments, const std::string& message)
{
    ezfadvance::SaveReaderOptions options;
    std::ostringstream errors;
    assert(ezfadvance::parseSaveReaderOptions(arguments, options, errors) ==
           ezfadvance::SaveReaderParseStatus::error);
    assert(errors.str().find(message) != std::string::npos);
}

} // namespace

int main()
{
    const auto inspection = parse({});
    assert(inspection.inspection_only);

    const auto extraction = parse({"--rom", "2", "--output", "save.sav"});
    assert(!extraction.inspection_only);
    assert(extraction.rom_number == 2);
    assert(extraction.output_path == "save.sav");

    const auto direct = parse(
        {"--save-bank", "0x0910", "--consecutive-bank", "2",
         "--output", "banks.sav"});
    assert(direct.save_bank->value() == 0x0910);
    assert(direct.consecutive_bank_count == 2);

    const auto write = parse(
        {"--write", "new.sav", "--backup", "old.sav",
         "--yes-really-write", "--save-bank", "0x0900"});
    assert(write.write_path == "new.sav");
    assert(write.backup_path == "old.sav");
    assert(write.authorize_write);

    const auto erase = parse(
        {"--erase", "--backup", "old.sav", "--save-bank", "0x0920",
         "--consecutive-bank", "2"});
    assert(erase.erase);
    assert(erase.backup_path == "old.sav");

    reject({"--rom", "0"}, "Bad --rom");
    reject({"--rom", "no"}, "Bad --rom");
    reject({"--save-bank", "0x0940"}, "Bad --save-bank");
    reject({"--consecutive-bank", "2"}, "requires --save-bank");
    reject({"--save-bank", "0x0930", "--consecutive-bank", "2"},
           "extends beyond");
    reject({"--rom", "1", "--save-bank", "0x0900",
            "--consecutive-bank", "1"}, "cannot be combined with --rom");
    reject({"--write", "save.sav"}, "requires --yes-really-write");
    reject({"--backup", "old.sav"}, "require --write");
    reject({"--write", "same.sav", "--backup", "same.sav",
            "--yes-really-write"}, "must be different");
    reject({"--erase", "--output", "save.sav"}, "cannot be combined");
    reject({"--unknown"}, "Unknown option");

    ezfadvance::SaveReaderOptions help;
    std::ostringstream errors;
    assert(ezfadvance::parseSaveReaderOptions({"--help"}, help, errors) ==
           ezfadvance::SaveReaderParseStatus::help);
}
