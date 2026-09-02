#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace ezfadvance {

class ReadOnlyCartridge;

struct Ez3ExtractionRequest {
    std::size_t rom_number = 0;
    std::string output_path;
};

class Ez3CardWorkflow final {
public:
    Ez3CardWorkflow(ReadOnlyCartridge& cartridge,
                    std::optional<Ez3ExtractionRequest> extraction,
                    bool verbose) noexcept;

    int run();

private:
    ReadOnlyCartridge& cartridge_;
    std::optional<Ez3ExtractionRequest> extraction_;
    bool verbose_ = false;
};

} // namespace ezfadvance
