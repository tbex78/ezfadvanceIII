#include "ezfadvance/save_file_store.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

int main()
{
    const std::string path = "save_file_store_test.tmp";
    std::remove(path.c_str());

    const ezfadvance::SaveFileStore files;
    std::ostringstream errors;
    const std::vector<std::uint8_t> first = {0x11, 0x22, 0x33};
    const std::vector<std::uint8_t> second = {0xaa, 0xbb};

    assert(files.writeNew(path, first, errors));
    assert(!files.writeNew(path, second, errors));
    assert(errors.str().find("Refusing to overwrite") != std::string::npos);

    errors.str("");
    errors.clear();
    assert(files.write(path, second, errors));
    std::ifstream input(path, std::ios::binary);
    const std::vector<std::uint8_t> actual{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    assert(actual == second);

    std::remove(path.c_str());
}
