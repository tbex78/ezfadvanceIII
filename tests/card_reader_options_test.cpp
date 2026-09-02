#include "ezfadvance/card_reader_options.hpp"

#include <cassert>
#include <sstream>
#include <string>
#include <vector>

namespace {

using ezfadvance::CardReaderAction;
using ezfadvance::CardReaderOptions;
using ezfadvance::CardReaderParseStatus;
using ezfadvance::CartridgeKind;

CardReaderOptions parse(std::vector<std::string> arguments)
{
    CardReaderOptions options;
    std::ostringstream errors;
    assert(ezfadvance::parseCardReaderOptions(arguments, options, errors) ==
           CardReaderParseStatus::run);
    assert(errors.str().empty());
    return options;
}

void expectRejected(std::vector<std::string> arguments,
                    const std::string& message)
{
    CardReaderOptions options;
    std::ostringstream errors;
    assert(ezfadvance::parseCardReaderOptions(arguments, options, errors) ==
           CardReaderParseStatus::error);
    assert(errors.str().find(message) != std::string::npos);
}

} // namespace

int main()
{
    const auto inspect = parse({});
    assert(!inspect.extract && !inspect.verbose && !inspect.rom_number);

    const auto unnumbered = parse({"--extract", "first.gba"});
    assert(unnumbered.extract && !unnumbered.rom_number);
    assert(unnumbered.output_path == "first.gba");

    const auto dumped = parse({"--dump", "dumped.gba"});
    assert(dumped.extract && !dumped.rom_number);
    assert(dumped.output_path == "dumped.gba");

    const auto numbered = parse(
        {"--verbose", "--extract", "2", "second.gba"});
    assert(numbered.extract && numbered.verbose);
    assert(numbered.rom_number == 2);
    assert(numbered.output_path == "second.gba");

    const auto numbered_dump = parse({"--dump", "3", "third.gba"});
    assert(numbered_dump.extract && numbered_dump.rom_number == 3);
    assert(numbered_dump.output_path == "third.gba");

    assert(ezfadvance::decideCardReaderAction(
               CartridgeKind::official_gba_rom, inspect).action ==
           CardReaderAction::inspect_official);
    assert(ezfadvance::decideCardReaderAction(
               CartridgeKind::official_gba_rom, unnumbered).action ==
           CardReaderAction::extract_official);

    const auto rejected_official = ezfadvance::decideCardReaderAction(
        CartridgeKind::official_gba_rom, numbered);
    assert(rejected_official.action == CardReaderAction::reject);
    assert(!rejected_official.error.empty());

    assert(ezfadvance::decideCardReaderAction(
               CartridgeKind::ez3_flash, inspect).action ==
           CardReaderAction::inspect_ez3);
    const auto default_ez3 = ezfadvance::decideCardReaderAction(
        CartridgeKind::ez3_flash, unnumbered);
    assert(default_ez3.action == CardReaderAction::extract_ez3);
    assert(default_ez3.rom_number == 1);
    const auto selected_ez3 = ezfadvance::decideCardReaderAction(
        CartridgeKind::ez3_flash, numbered);
    assert(selected_ez3.action == CardReaderAction::extract_ez3);
    assert(selected_ez3.rom_number == 2);

    expectRejected({"--verbose"}, "--verbose");
    expectRejected({"--extract"}, "incomplete");
    expectRejected({"--dump"}, "incomplete");
    expectRejected({"--extract", "0", "bad.gba"}, "Bad ROM number");
    expectRejected({"--extract", "nope", "bad.gba"}, "Bad ROM number");
    expectRejected({"--extract", "1", "a.gba", "extra"}, "Unknown option");
    expectRejected({"--extract", "a.gba", "--extract", "b.gba"},
                   "Invalid");
    expectRejected({"--extract", "a.gba", "--dump", "b.gba"},
                   "Invalid");

    CardReaderOptions help;
    std::ostringstream help_errors;
    assert(ezfadvance::parseCardReaderOptions({"--help"}, help, help_errors) ==
           CardReaderParseStatus::help);
}
