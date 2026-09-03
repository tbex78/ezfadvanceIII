#include "ezfadvance/writer_options.hpp"

#include <cassert>
#include <sstream>
#include <string>
#include <vector>

namespace {

ezfadvance::WriterParseResult parse(
    const std::vector<std::string>& arguments,
    ezfadvance::WriterOptions& options,
    std::ostringstream& errors)
{
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (const std::string& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    return ezfadvance::WriterOptions::parse(
        static_cast<int>(argv.size()), argv.data(), options, errors);
}

void expectRejected(const std::string& argument,
                    const std::string& expected_error)
{
    ezfadvance::WriterOptions options;
    std::ostringstream errors;
    const auto result = parse({"writer", argument}, options, errors);
    assert(!result.ok);
    assert(errors.str().find(expected_error) != std::string::npos);
}

} // namespace

int main()
{
    {
        ezfadvance::WriterOptions options;
        std::ostringstream errors;
        const auto result = parse(
            {"writer", "--map1=4", "--map120=6", "--type10=3",
             "--title1=Menu Name", "rom.gba"},
            options, errors);
        assert(result.ok);
        assert(errors.str().empty());
        assert(options.mapping_overrides[0] == 4);
        assert(options.mapping_overrides[119] == 6);
        assert(options.type_overrides[9] == 3);
        assert(options.title_overrides[0] == "Menu Name");
        assert(options.rom_paths.size() == 1);
        assert(options.rom_paths[0] == "rom.gba");
    }

    expectRejected("--map0=4", "Bad mapping override slot");
    expectRejected("--map121=4", "Bad mapping override slot");
    expectRejected("--mapbanana=4", "Bad mapping override slot");
    expectRejected("--map1=banana", "Bad mapping override value");
    expectRejected("--map1=256", "Bad mapping override value");
    expectRejected("--type1=-1", "Bad type override value");
    expectRejected("--map999999999999999999999999999999999=4",
                   "Bad mapping override slot");
    expectRejected("--type1=99999999999999999999999999999999",
                   "Bad type override value");
    expectRejected("--title0=Name", "Bad title override slot");
    expectRejected("--title121=Name", "Bad title override slot");
    expectRejected("--title1=", "Bad title override value");
    expectRejected("--title1=1234567890abcdefg", "Bad title override value");

    {
        ezfadvance::WriterOptions options;
        std::ostringstream errors;
        const auto result = parse({"writer", "--unknown"}, options, errors);
        assert(!result.ok);
        assert(result.show_usage);
        assert(errors.str().find("Unknown option") != std::string::npos);
    }
}
