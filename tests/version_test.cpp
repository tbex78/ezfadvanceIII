#include "ezfadvance/version.hpp"

#include <cassert>
#include <sstream>

int main()
{
    char program[] = "tool";
    char version[] = "--version";
    char extra[] = "extra";
    char* request[] = {program, version};
    char* invalid_request[] = {program, version, extra};

    assert(ezfadvance::isVersionRequest(2, request));
    assert(!ezfadvance::isVersionRequest(3, invalid_request));
    assert(ezfadvance::project_version == "0.14.0");

    std::ostringstream output;
    ezfadvance::printVersion(output, "tool");
    assert(output.str() == "tool 0.14.0\n");
}
