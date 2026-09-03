#pragma once

namespace ezfadvance {

// Owns save-reader command-line preflight and workflow dispatch. The process
// entry point remains a minimal composition boundary.
class SaveReaderApplication final {
public:
    static int run(int argc, char** argv);
};

} // namespace ezfadvance
