#pragma once

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>

namespace ezfadvance {

const char* hostPlatformName() noexcept;
bool stdoutIsTerminal() noexcept;
std::optional<std::size_t> stdoutTerminalWidth() noexcept;
std::string fitProgressToTerminal(std::string line);
void beginProgressLine(std::ostream& output);

} // namespace ezfadvance
