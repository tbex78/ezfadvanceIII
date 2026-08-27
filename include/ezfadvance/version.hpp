#pragma once

#include <ostream>
#include <string_view>

namespace ezfadvance {

inline constexpr std::string_view project_version = "0.10.2";

inline bool isVersionRequest(int argc, char** argv)
{
    return argc == 2 && std::string_view(argv[1]) == "--version";
}

inline void printVersion(std::ostream& output, std::string_view program_name)
{
    output << program_name << ' ' << project_version << '\n';
}

} // namespace ezfadvance
