#include "ezfadvance/platform.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <io.h>
#else
#  include <sys/ioctl.h>
#  include <unistd.h>
#endif

namespace ezfadvance {

const char* hostPlatformName() noexcept
{
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__) && defined(__MACH__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#elif defined(__FreeBSD__)
    return "FreeBSD";
#elif defined(__OpenBSD__)
    return "OpenBSD";
#elif defined(__NetBSD__)
    return "NetBSD";
#elif defined(__DragonFly__)
    return "DragonFly BSD";
#else
    return "Unknown OS";
#endif
}

bool stdoutIsTerminal() noexcept
{
#if defined(_WIN32)
    if (_isatty(_fileno(stdout)) == 0) return false;
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
#else
    return ::isatty(STDOUT_FILENO) != 0;
#endif
}

std::optional<std::size_t> stdoutTerminalWidth() noexcept
{
    if (!stdoutIsTerminal()) return std::nullopt;
#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO info {};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info) == 0)
        return std::nullopt;
    const SHORT width = static_cast<SHORT>(info.srWindow.Right -
                                           info.srWindow.Left + 1);
    if (width < 2) return std::nullopt;
    return static_cast<std::size_t>(width);
#else
    struct winsize terminal_size {};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &terminal_size) != 0 ||
        terminal_size.ws_col < 2)
        return std::nullopt;
    return static_cast<std::size_t>(terminal_size.ws_col);
#endif
}

std::string fitProgressToTerminal(std::string line)
{
    const auto width = stdoutTerminalWidth();
    if (!width) return line;
    const std::size_t maximum = *width - 1;
    if (line.size() > maximum) line.resize(maximum);
    return line;
}

void beginProgressLine(std::ostream& output)
{
    output << '\r';
#if !defined(_WIN32)
    if (stdoutIsTerminal()) output << "\033[2K";
#endif
}

} // namespace ezfadvance
