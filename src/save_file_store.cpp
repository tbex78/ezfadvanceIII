#include "ezfadvance/save_file_store.hpp"

#include <fstream>

namespace ezfadvance {
namespace {

bool writeFile(const std::string& path,
               const std::vector<std::uint8_t>& data,
               std::ostream& errors,
               const char* kind)
{
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        errors << "Could not create " << kind << " file: " << path << '\n';
        return false;
    }
    output.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
    if (!output) {
        errors << "Failed while writing " << kind << " file: " << path
               << '\n';
        return false;
    }
    return true;
}

} // namespace

bool SaveFileStore::write(const std::string& path,
                          const std::vector<std::uint8_t>& data,
                          std::ostream& errors) const
{
    return writeFile(path, data, errors, "output");
}

bool SaveFileStore::writeNew(const std::string& path,
                             const std::vector<std::uint8_t>& data,
                             std::ostream& errors) const
{
    std::ifstream existing(path, std::ios::binary);
    if (existing.good()) {
        errors << "Refusing to overwrite existing backup file: " << path
               << '\n';
        return false;
    }
    existing.close();
    return writeFile(path, data, errors, "backup");
}

} // namespace ezfadvance
