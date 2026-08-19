#if defined(_WIN32)
#error "Native Windows builds are intentionally unsupported. Use macOS, Linux, BSD, or a Linux VM on Windows."
#endif

#if defined(__has_include)
#  if __has_include(<libusb-1.0/libusb.h>)
#    include <libusb-1.0/libusb.h>
#  elif __has_include(<libusb.h>)
#    include <libusb.h>
#  else
#    error "libusb header not found. Install the libusb development package."
#  endif
#else
#  include <libusb-1.0/libusb.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "ezfadvance/usb_device.hpp"
#include "ezfadvance/cartridge_format.hpp"
#include "ezfadvance/protocol.hpp"
#include "ezfadvance/verification_session.hpp"
#include "ezfadvance/writer_options.hpp"
#include "ezfadvance/verification_policy.hpp"

static constexpr size_t PROGRAM_BLOCK = 0x10000; // 64 KiB
static constexpr size_t FLASH_WINDOW_SIZE = 0x800000; // 8 MiB local program window
static constexpr size_t CARD_HALF_SIZE = 0x1000000;   // 128 Mbit / 16 MiB boundary
static constexpr size_t MAX_CARD_IMAGE = 0x2000000;   // 256 Mbit / 32 MiB card
static constexpr unsigned USB_TIMEOUT_MS = 15000;

static constexpr const char* host_platform_name()
{
#if defined(__APPLE__) && defined(__MACH__)
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
    return "Unix-like OS";
#endif
}


static std::string progress_duration(double seconds)
{
    if (seconds < 0.0) seconds = 0.0;
    const uint64_t total_seconds = static_cast<uint64_t>(seconds + 0.5);
    const uint64_t hours = total_seconds / 3600;
    const uint64_t minutes = (total_seconds % 3600) / 60;
    const uint64_t secs = total_seconds % 60;

    std::ostringstream oss;
    if (hours != 0) {
        oss << hours << ':' << std::setw(2) << std::setfill('0') << minutes
            << ':' << std::setw(2) << secs;
    } else {
        oss << minutes << ':' << std::setw(2) << std::setfill('0') << secs;
    }
    return oss.str();
}

class ProgressBar
{
public:
    ProgressBar(std::string label, uint64_t total, bool byte_units, bool enabled)
        : label_(std::move(label)),
          total_(total),
          byte_units_(byte_units),
          enabled_(enabled),
          started_(std::chrono::steady_clock::now())
    {
    }

    ~ProgressBar()
    {
        if (enabled_ && drew_ && !finished_)
            std::cout << '\n';
    }

    void update(uint64_t completed)
    {
        if (!enabled_) return;
        if (completed > total_) completed = total_;

        const auto now = std::chrono::steady_clock::now();
        const double elapsed =
            std::chrono::duration<double>(now - started_).count();
        const double ratio =
            total_ != 0 ? static_cast<double>(completed) /
                          static_cast<double>(total_) : 1.0;

        static constexpr size_t BAR_WIDTH = 32;
        size_t filled = static_cast<size_t>(ratio * BAR_WIDTH);
        if (filled > BAR_WIDTH) filled = BAR_WIDTH;

        std::ostringstream oss;
        oss << label_ << " [";
        for (size_t i = 0; i < BAR_WIDTH; ++i)
            oss << (i < filled ? '=' : (i == filled && completed < total_ ? '>' : ' '));
        oss << "] " << std::fixed << std::setprecision(1)
            << (ratio * 100.0) << "%  ";

        if (byte_units_) {
            const double done_mib =
                static_cast<double>(completed) / (1024.0 * 1024.0);
            const double total_mib =
                static_cast<double>(total_) / (1024.0 * 1024.0);
            oss << std::setprecision(2) << done_mib << '/'
                << total_mib << " MiB";
        } else {
            oss << completed << '/' << total_;
        }

        if (elapsed > 0.0 && completed != 0) {
            const double rate = static_cast<double>(completed) / elapsed;
            oss << "  ";
            if (byte_units_) {
                oss << std::setprecision(1) << (rate / 1024.0) << " KiB/s";
            } else {
                oss << std::setprecision(1) << rate << "/s";
            }

            const double remaining =
                rate > 0.0 ? static_cast<double>(total_ - completed) / rate : 0.0;
            oss << "  elapsed " << progress_duration(elapsed)
                << "  ETA " << progress_duration(remaining);
        }

        const std::string line = oss.str();
        std::cout << '\r' << line;
        if (last_width_ > line.size())
            std::cout << std::string(last_width_ - line.size(), ' ');
        std::cout << std::flush;
        last_width_ = line.size();
        drew_ = true;

        if (completed >= total_) {
            std::cout << '\n';
            finished_ = true;
        }
    }

private:
    std::string label_;
    uint64_t total_;
    bool byte_units_;
    bool enabled_;
    bool drew_ = false;
    bool finished_ = false;
    size_t last_width_ = 0;
    std::chrono::steady_clock::time_point started_;
};

// ezfadvanceIII multi-ROM writer 0.7.17 for macOS, Linux and BSD.
//
// 0.6.2 removes hard-coded project-version text from runtime banners.
// synchronization. Verification behavior remains evidence-bounded:
//   * every constructed image below 8 MiB uses the capture-proven status-only
//     partial-BANK0 preparation followed by full linear 0x91 verification;
//   * exact 8, 16, 24 and 32 MiB retain their capture-proven transitions;
//   * the Fire Emblem-style tiny tail immediately above 16 MiB retains its
//     dedicated capture-proven path;
//   * other partial higher-window geometries remain verification-skipped until
//     an original-EZ3Manager capture proves their linear-read mapping.
// The skip diagnostic and usage notes now state these boundaries explicitly.
// No new higher-window selector is guessed in this release.
//
// 0.5.17 incorporates the new sub-8-MiB captures:
//   * 2MB.pcap is a single 1-MiB ROM image;
//   * 2_2MB.pcap is two 1-MiB ROMs / 2 MiB of ROM data total.
// Together with 4MB.pcap they prove that partial first-window verification
// uses only the normal flash status/read-state sequence (FFFF,04,00,00)
// followed by ordinary linear 0x91 reads. No 0x0020/0x0040 selector is sent
// for these partial (<8-MiB) geometries.
// The capture-observed delays before the first read vary in 15.625-ms USBPcap
// timestamp quanta, so no fixed 50-ms application delay is imposed.
// The new captures also prove two extent rules used by original EZ3Manager:
//   * program extent is rounded up to a 0x100-byte boundary;
//   * sub-8-MiB verification extent is rounded up to a full 0x10000-byte block
//     and bytes beyond the programmed image are expected to remain 0xFF.
// Finally, EEPROM_V124 is now known to map to both catalog map 4 and map 5 in
// different captures. Until a generic EEPROM-capacity discriminator is found,
// EEPROM ROMs require an explicit --mapN=4 or --mapN=5 override.
//
// 0.5.16 adds exact 4-MiB / 32-Mbit post-program verification from 4MB.pcap.
// The original EZ3Manager sequence after the final program block is:
//   FFFF, 04, 00, 00
// followed by a capture-observed 46.875-ms quiet interval, then ordinary
// linear 0x91 reads from word address 0x000000 through 0x1F8000.
// USBPcap timestamps are quantized at 15.625 ms, so the implementation uses
// a 50-ms settle before the first read. The capture contains 64 program blocks
// and 64 verification reads, and all captured payload bytes match.
//
// 0.5.15 narrows lower-window verification to what is actually captured.
// Exact 8 MiB / 64 Mbit uses the proven 0x0040 linear-read transition.
// Smaller/other partial first-window images are still erased and programmed
// normally, but full post-write read-back verification is conservatively
// skipped because no exact single-4-MiB (or other partial-first-window)
// EZ3Manager verification capture is currently available. This avoids turning
// an unproven read mapping into a false WRITE/VERIFY failure.
//
// 0.5.14 fixes the <=8-MiB post-program verification transition.
// The previous branch sent only flash_status_sequence() before linear 0x91 reads.
// Two independent original-EZ3Manager captures (4MiB-4MiB.pcap and the
// 8-MiB FLASH_V121_FLASH512K.pcap) show the required 64-Mbit transition:
//   status, 55AA, 0200, 0040, 0000, ~125 ms, AA55, 0000 x3,
//   AA,55,06, status, then 55AA,0000,0000,0000.
// This also addresses the observed 4-MiB single-ROM verification failure where
// byte 0 read back as 0x80 instead of the programmed ARM branch byte.
//
// 0.5.13 changes console reporting only; USB protocol/image behavior is unchanged:
//   * default destructive writes use live in-place progress bars for erase,
//     program, and read-back verification;
//   * --verbose restores per-operation diagnostics such as
//       program card byte 0x00110000 local byte 0x110000 length 0x10000
//       single data request: 1.129 s, 56.7 KiB/s
//     plus per-sector erase and per-block verification lines;
//   * progress bars report percentage, completed amount, average throughput,
//     elapsed time, and ETA.
//
// 0.5.12 removes the artificial five-ROM input limit. The writer now validates
// capacity rather than imposing a small fixed ROM count:
//   * total input ROM file bytes must not exceed 32 MiB / 256 Mbit;
//   * the packed image plus loader must still fit the physical 32-MiB card;
//   * the embedded loader has 120 structurally available 28-byte catalog slots,
//     which is enforced as a safety bound rather than a claimed hardware/menu
//     limit;
//   * override syntax now supports multi-digit slots (for example --type10=3
//     and --map10=6).
// 4_4_4_4_8_8MB.pcap independently proves six active catalog entries, the same
// 0x7080 loader, the same 125 relocations, the >=4-ROM zero-tail rule, and the
// existing full-32-MiB erase/program/verify path.
//
// 0.5.11 adds capture-proven five-ROM and exact-24-MiB behavior from
// 4_4_4_4_8MB.pcap:
//   * five active 28-byte catalog entries are accepted;
//   * the existing 0x7080 multi-ROM loader remains unchanged and still
//     relocates at exactly 125 addresses;
//   * the same final 26-byte zero tail seen with four ROMs is also used with
//     five ROMs, so the zero-tail rule is now applied for 4 or 5 ROMs;
//   * a 24-MiB image uses three 8-MiB erase/program windows;
//   * exact 24-MiB post-write verification is now capture-proven: after
//     window-2 programming, EZ3Manager transitions through the 0x00C0 mapping
//     sequence and then performs linear 0x91 reads across all 24 MiB.
// No new loader blob, save-map rule, packing rule, or USB framing is introduced.
//
// 0.5.10 is a Unix-like portability release. Protocol/image behavior is unchanged:
//   * libusb header detection accepts either <libusb-1.0/libusb.h> or <libusb.h>;
//   * Linux-only automatic kernel-driver detach is isolated behind __linux__;
//   * std::filesystem is no longer required for deriving catalog names;
//   * platform reporting covers macOS, Linux, FreeBSD, OpenBSD, NetBSD and DragonFly BSD;
//   * native Windows builds are intentionally unsupported; Windows users should use
//     a Linux VM with USB passthrough for VID 0x0E6A / PID 0x5088.
//   * warning wording now says "embedded save-library signature" because a manually
//     SRAM-patched ROM may retain its original FLASH/EEPROM marker (as observed with FFTA).
//
// 0.5.9 changes warning/help text only. It explicitly states that this writer never
// patches or converts ROM save routines; any SRAM conversion is a separate, manual
// user action performed with an external save-patching tool before running this writer.
// Protocol, packing, loader, catalog classification, USB write, and verification
// behavior are unchanged from 0.5.8.
//
// 0.5.8 adds original-manager EEPROM catalog support from TOF-EEPROM.pcap:
//   * Tales of Phantasia (16 MiB, EEPROM_V124) uses catalog type 1 / map 5.
//   * the existing single-ROM loader template is unchanged; relocated to the
//     captured 0x00D49020 address it matches all 0x660 loader bytes exactly.
//   * no EEPROM-specific ROM patch is visible in the capture: outside the
//     patched entry branch and inserted loader, all captured ROM bytes match
//     the original dump, including the EEPROM_V124 library region.
//   * EEPROM_V... is therefore mapped to 5 instead of being rejected.
//   * the single-ROM internal-FF search reserves a full multi-loader-sized gap
//     and advances to the next 16-byte boundary inside the run; this reproduces
//     Tales' captured 0x00D49020 placement and avoids an earlier too-small gap.
//
// 0.5.7 makes each non-SRAM save warning interactive. After the warning the
// user must explicitly answer y/yes to continue. n/no, Enter, EOF, or any other
// response aborts safely before USB access or cartridge modification.
//
// 0.5.6 adds a per-ROM save-compatibility warning for non-SRAM save libraries.
// FLASH_V/FLASH512_V/FLASH1M_V and EEPROM_V ROMs now emit a prominent warning
// advising an SRAM save patch when working save games are required on the
// EZ-Flash Advance III. SRAM/SRAM_F and ROMs without a recognized non-SRAM
// save-library marker do not emit this warning. EEPROM is now supported through
// the capture-derived map-5 path added in 0.5.8.
//
// 0.5.5 replaces the old save-signature-based catalog type classifier with
// the ROM-size-class rule exposed by 4MiB-4MiB.pcap and 4_4_8MiB.pcap and
// confirmed on real hardware with the 0.5.4 command-line overrides:
//   * 32 MiB -> type 0, 16 MiB -> 1, 8 MiB -> 2, 4 MiB -> 3, ...
//     down to 64 KiB -> type 9.
//   * non-power-of-two files use the next power-of-two size class, matching
//     the existing multi-ROM placement allocator.
//   * the mapping/config byte remains independent of ROM size: standard GBA
//     FLASH save-library families use map 6; other non-EEPROM ROMs use map 3.
//   * captured FLASH_V121/V124/V126 and FLASH512_V130 cases all fit map 6.
// EEPROM handling was still restricted in 0.5.5; 0.5.8 replaces that restriction
// with the capture-derived map-5 path. Packing, loader forms, USB write geometry,
// --skip-verify and four-ROM support remain otherwise unchanged.
//
// 0.5.4 adds capture-proven four-ROM support from 8_8_8_8MiB.pcap and
// removes the on-disk intermediate image export. The constructed image remains
// entirely in memory for dry-run layout inspection, programming and optional
// verification. --yes-really-write remains the destructive-write safety gate.
//
// 8_8_8_8MiB.pcap proves:
//   * four active 28-byte catalog entries are supported;
//   * equal-size 8-MiB ROMs preserve their input order;
//   * physical starts 0x0000000, 0x0800000, 0x1000000, 0x1800000;
//   * the normal 0x7080 multi-ROM loader is used;
//   * for the four-ROM form, loader bytes 0x7066..0x707F are zero-filled;
//   * full 32-MiB erase/program/verify uses the already-proven four-window path.
//
// 0.5.3 adds --skip-verify. When supplied with --yes-really-write, erase and
// programming are unchanged, but the post-write ROM read-back comparison is
// skipped. A short non-readback flash status/reset cleanup is still sent so
// the bridge is not intentionally left in program-command state.
//
//
// 0.5.2 replaces the old input-order multi-ROM packing with the ordering and
// placement behavior now proven by original EZ3Manager USB captures:
//   * ROMs are stable-sorted by descending file size; equal-size ROMs keep
//     their original relative order.
//   * later ROMs are aligned to their ROM size class, allowing them to reuse
//     only trailing 0xFF padding from earlier/larger ROM files.
//   * the multi-ROM loader/menu is placed in the first sufficiently large
//     16-byte-aligned 0xFF run in the completed packed image when possible.
//
// piano-megamanz.pcap proves MegaManZ is reordered ahead of Piano, Piano is
// placed at 0x007F0000 inside MegaManZ's trailing erased 64-KiB region, and
// the loader is embedded at 0x002CC420. The resulting image is exactly 8 MiB.
//
// 8_8_16MiB.pcap proves stable descending-size ordering for three ROMs:
// Fire Emblem (16 MiB), Advance Wars (8 MiB), MegaManZ (8 MiB), preserving
// Advance Wars before MegaManZ because their sizes are equal. Their captured
// starts are 0x0000000, 0x1000000 and 0x1800000; the loader is embedded in
// Advance Wars at 0x15C2360.
//
// The experimental 0.5.1 local-window verifier is removed: real hardware
// proved its BANK1 read still returned window-0 data. Full read-back verify is
// retained only for capture-proven geometries (<=8 MiB, exact 16 MiB, exact
// 24 MiB, exact 32 MiB, and the tiny Fire-Emblem-style tail immediately above 16 MiB).
// Other partial higher-window images are written normally but are not falsely
// reported as verification failures.
//
// The preventive full-card wipe recommendation remains informational only;
// the writer never runs ezfadvanceIII_wipe_card automatically.
//
// v35 removed the game-specific AFXP/FFTA catalog-name override. Catalog names
// are always derived through the normal generic naming path.
//
// v34 capture-derived support preserved:
//   * fireemblem.pcap proves a full 16-MiB ROM with no internal FF loader
//     slot places the single-ROM loader at card byte 0x01000010, preceded
//     by 16 zero bytes, and programs a 0x700-byte block in window 0x0080.
//   * 256MBits-rom.pcap proves all four 8-MiB program/erase windows:
//       0..8 MiB   -> base/default
//       8..16 MiB  -> 0x0040
//       16..24 MiB -> 0x0080
//       24..32 MiB -> 0x00C0
//     and proves full 32-MiB linear verification.
//   * Fire Emblem's SRAM_F_V102 catalog entry is type 1 / mapping 3.
//   * Kingdom Hearts' SRAM_F_V103 catalog entry is type 0 / mapping 3.
// Existing v33 cartridge-readiness, EEPROM safety, loader templates, and all
// previously hardware-proven <=16-MiB behaviors are preserved.
static constexpr unsigned COMMAND_DATA_SETTLE_US = 750; // exact legacy DLL busy-wait
static constexpr unsigned READINESS_ATTEMPTS = 5;
static constexpr auto READINESS_RETRY_DELAY = std::chrono::milliseconds(100);

static constexpr uint32_t GBA_ROM_BASE = 0x08000000u;

// Single-ROM loader template captured from writeromonemptycard.pcap.
// Original load address: 0x0000D3B0, length 0x660.
// Relocation algorithm was validated byte-for-byte against the independent
// GBA-Notepad single-ROM capture.
static constexpr uint32_t SINGLE_TEMPLATE_BASE = 0x0000D3B0u;
static constexpr size_t SINGLE_TEMPLATE_LEN = 0x660;
static constexpr size_t SINGLE_HEADER_OFF = 0x4E8;
static constexpr size_t SINGLE_HEADER_COUNT2_OFF = 0x4F6;
static constexpr size_t SINGLE_ENTRY_OFF = 0x4F8;

// Multi-ROM loader/menu template captured from add3rom.pcap.
// v24 restores the exact v4-v8 capture-derived bytes; v9-v23 had 25
// accidental byte changes in this embedded template.
// Original load address: 0x0004A880, length 0x7080.
// The 28 bytes lost at the USBPcap 64 KiB snaplen boundary were recovered
// directly from a cartridge written by the original Windows EZ3Manager.
// They are patched explicitly at loader offset 0x5764 below.
// Everything else is directly capture-derived.
static constexpr uint32_t MULTI_TEMPLATE_BASE = 0x0004A880u;
static constexpr size_t MULTI_TEMPLATE_LEN = 0x7080;
static constexpr size_t MULTI_HEADER_OFF = 0x475E;
static constexpr size_t MULTI_ENTRY_OFF = 0x476E;
// The repeated 28-byte catalog-slot region ends at loader offset 0x548E.
// 4_4_4_4_8MB.pcap and 4_4_4_4_8_8MB.pcap prove entries 1..6 use this same
// structure. The base template contains empty sentinel entries beyond those
// used by the capture, so the writer may populate additional slots safely as
// long as it never crosses into the next loader data structure.
static constexpr size_t MULTI_CATALOG_END_OFF = 0x548E;
static constexpr size_t MULTI_CATALOG_MAX_ENTRIES =
    (MULTI_CATALOG_END_OFF - MULTI_ENTRY_OFF) / 28;
static_assert(MULTI_ENTRY_OFF + MULTI_CATALOG_MAX_ENTRIES * 28
              == MULTI_CATALOG_END_OFF,
              "multi-ROM catalog region must contain whole 28-byte entries");
static_assert(MULTI_CATALOG_MAX_ENTRIES == 120,
              "unexpected multi-ROM catalog slot count");

// kMultiTemplateB64 came from the original three-ROM capture, so its first
// three catalog slots contain captured entries. Later slots are already empty
// sentinels; additional ROMs overwrite those slots normally.
static constexpr size_t MULTI_TEMPLATE_POPULATED_ENTRY_SLOTS = 3;

// piano-bios.pcap proves that the original manager uses a slightly shorter
// two-ROM form of the multi-ROM loader/menu image.
//
// For two ROMs:
//   - the programmed loader/menu extent is 0x6F80 bytes, not 0x7080;
//   - bytes 0x6F26..0x6F7F are left erased (0xFF);
//   - the unused third catalog slot is encoded as:
//       00 + fifteen ASCII spaces + twelve 00 bytes.
//
// Absolute addresses for Piano + Bios_Dumper with loader @ 0x1A880:
//   erased tail begins 0x217A6
//   original manager stops programming at 0x21800.
static constexpr size_t MULTI_TWO_ROM_LEN = 0x6F80;
static constexpr size_t MULTI_TWO_ROM_ERASED_TAIL_OFF = 0x6F26;

// 8_8_8_8MiB.pcap, 4_4_4_4_8MB.pcap, and 4_4_4_4_8_8MB.pcap prove that
// the four-, five-, and six-ROM forms keep the full 0x7080 loader and
// zero-fill its final 26 bytes.
// The three-ROM capture leaves these bytes FF.
static constexpr size_t MULTI_FOUR_PLUS_ROM_ZERO_TAIL_OFF = 0x7066;
static constexpr size_t MULTI_FOUR_PLUS_ROM_ZERO_TAIL_LEN = 0x1A;

// Exact 28-byte loader gap recovered from a real cartridge programmed by
// the original Windows software (absolute 0x1FFE4 for Piano+Bios_Dumper,
// loader-relative offset 0x5764).  These bytes were invisible in USBPcap
// because each 64-KiB transfer was truncated by the capture snaplen.
static constexpr size_t MULTI_CAPTURE_GAP_OFF = 0x5764;
static constexpr std::array<uint8_t, 28> MULTI_CAPTURE_GAP_BYTES = {
    0x00,0x00,0x00,0x06,  // 0x06000000
    0x00,0x00,0x00,0x06,  // 0x06000000
    0x00,0x00,0x00,0x06,  // 0x06000000
    0x00,0x00,0x00,0x05,  // 0x05000000
    0x00,0x02,0x00,0x05,  // 0x05000200
    0x00,0x00,0x00,0x07,  // 0x07000000
    0x00,0x00,0x01,0x06   // 0x06010000
};

static const char kSingleTemplateB64[] =
    "BgAA6gBiYmJiAAAAAAAAAOgEAAAAAAAAAAAAAAAAAAAGAADqAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABIAoOMA8CnhxNCf"
    "5R8AoOMA8CnhvNCf5QEAj+IQ/y/hLkkvSAhgL0hAAQrSeEZAAQzTQCMbA9oBFhyRAADwQPgwR0AhCQPIAQDwMPgDIAAGJUkA8Cv4"
    "JUglSQkaAPAm+CRJJUolTADwKvglSSVKJkwA8CX4JUomSVMaAtAlSgDwH/glSSVKJkwA8Bn4JUomSVMaAtAlSgDwE/gAIAAhJEue"
    "RiRLGEcAR8BGwEYDIokYkUMD0AAiBMAEOfzRcEejGgMgGxiDQwPQAckBwgQ7+9FwRwAAoH8AAwB/AAP8fwADAAUAA7DTAAjweQAA"
    "TAYAA/gGAAMQ2gAI+AYAA/gGAAPE2AAIAAUAA0wGAAMQ2gAIENoACPgGAAMQ2gAIAAAAAgAAAAIQ2gAIENoACAAAAALw0wAIKNUA"
    "CA3AoOFw2C3poDGf5aBhn+UEsEziBACg4wAwhuVpAADrVTzg4wBQoOMCw6DjqjBD4rAwzOEA4JblAFCM5bBQzOETIN7ltjHe4QMo"
    "guBkMZ/lVRyg4wAgg+UOBKDjVTDg46oQgeKwEMzhVSCg4wBQjOWwUMzhADDA5QwwoOMAIMDlATDA5QBQwOUAUMDlFDDe5QMwQ+L/"
    "MBPiBUCg4RghnwUCO6ADPwAACgEAU+MIIZ8FATugAzsAAAoCAFPj+CCfBQY8oAM3AAAKAwBT4+ggnwWwUMIBAgAACgQAU+PYIJ8V"
    "LgAACrAw0uEBMIPjABCW5bAwwuG6IdHhuDHR4bwAn+UCOIPgAjOD4gAwgOUAMKDjAROg4wMgoOEBMIPiAzig4VUIU+MBIMHkQzig"
    "4fn//5oBE6DjQy+g47AQgeKwMKDjAyCC4gDAoOMBMIPiAzig4SMIUuEBwMHkQzig4fn//yoBE6DjfzDg4wAwweUBIKDhATCg4yEQ"
    "geIAMMHlNzDC5ScwwuUxMMLlNDCf5QAAkOUP4KDhE/8v4XBoG+ke/y/hFCCf5QE6oOOwMMLhzP//6qjYAAhgBgADgAYAA7AGAAOg"
    "BgADEAYAAwTgLeVVPKDjAMCg4wIjoOOqMIPisDDC4Q4UoOMAwILl/wAA4rDAwuFVMODjVSCg4wzgoOEAMMHlACDB5QEAweUAwMHl"
    "AMDB5QTgneQe/y/hHv8v4QEzoOOCP4PiACCg47AQ0+EcAJ/lsCDD4RgQn+UBMILiAhGA5/8gA+INAFLj+v//mh7/L+HABgADQNcA"
    "CAHDoOMCzIzisCDc4f8AAOIBMKDjEyCC4Wwwn+UCAFDjsCDM4QARg+cBI6DjBgAAigEAUOMRAAAKCwAAygAAUOO0MNIBCDCDAwUA"
    "AAoBI6Djgi+C4rAw0uEBMIPjsDDC4R7/L+G0MMLh9///6gIAUOP1//8atDDS4SAwg+P4///qtDDS4RAwg+P1///qwAYAAwEToOMC"
    "HIHisCDR4f88oOP/AADi/jCD4hMgAuACAFDjsCDB4QEjoOMGAACKAQBQ4w8AAAoJAADKAABQ47Qw0gEIMMMDAwAACjAgn+UwMJ/l"
    "ACGD5x7/L+G0MMLh+f//6gIAUOP3//8atDDS4SAww+P4///qtDDS4RAww+P1///qQNcACMAGAAMBAAAAAAAAAAAAAAAAAAEAcGlh"
    "bm8AAAAAAAAAAAAAAAAAAAkDAAAAwAAAAAEjoOMAMrLluBDS4QAAT+EHQC3pAQCg47gAwuEjGAPg5MCf5QEAEeIlAAAaBMCM4gIA"
    "EeIiAAAaBMCM4gQAEeIfAAAaBMCM4ggAEeIcAAAaBMCM4hAAEeIZAAAaBMCM4iAAEeIWAAAaBMCM4kAAEeITAAAaBMCM4oAAEeIQ"
    "AAAaBMCM4gEMEeINAAAaBMCM4gIMEeIKAAAaBMCM4gELEeIHAAAaBMCM4gILEeIEAAAaBMCM4gEKEeIBAAAaBMCM4gIKEeKyAMLh"
    "ADAP4d8ww+MfMIPjA/Ap4QAAnOUAQC3pAOCP4hD/L+EAQL3oADAP4d8ww+OSMIPjA/Ap4QdAvei4EMLhAPBp4R7/L+HABgADVTzg"
    "4wIToOOqMEPisDDB4Rwgn+UcMJ/lAMCS5bAg0+EAwIHlsCDB4QAwoOEA8KDhHv8v4YAGAAOwBgAD";

static const char kMultiTemplateB64[] =
    "BgAA6gBiYmJiAAAAAAAAAF5HAAAAAAAAAAAAAAAAAAAGAADqAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABIAoOMA8CnhxNCf"
    "5R8AoOMA8CnhvNCf5QEAj+IQ/y/hLkkvSAhgL0hAAQrSeEZAAQzTQCMbA9oBFhyRAADwQPgwR0AhCQPIAQDwMPgDIAAGJUkA8Cv4"
    "JUglSQkaAPAm+CRJJUolTADwKvglSSVKJkwA8CX4JUomSVMaAtAlSgDwH/glSSVKJkwA8Bn4JUomSVMaAtAlSgDwE/gAIAAhJEue"
    "RiRLGEcAR8BGwEYDIokYkUMD0AAiBMAEOfzRcEejGgMgGxiDQwPQAckBwgQ7+9FwRwAAoH8AAwB/AAP8fwADAAUAA4CoBAjweQAA"
    "TAYAAzoNAANI/wQIPA0AAwAOAAP8/QQIAAUAA0wGAAMMAAUIDAAFCAAOAAMMAAUIAAAAAgAAAAIMAAUIDAAFCAAAAALAqAQIHLkE"
    "CAIooOH/EAHiEEAt6UIooOEAQKDhAwBR4wHxn5cbAADqKKoECHiqBAiAqgQIiKoECGAwn+UA4JPlAjig4SMIoOEAEKDjAABR4QoA"
    "ACoBwKDhATCB4gM4oOFDGKDhASig4aw3oOEiCFDhAsCg4bQgk+G+IIPh9f//ihBAvege/y/hFDCf5er//+oQMJ/l6P//6gwwn+Xm"
    "///qPA0AA0ANAANEDQADSA0AA/8AAOLwQS3p/yAC4v+AAeL/YAPiHwBQ4wDxn5d6AADqQKsECLyrBAjEqwQIzKsECNSrBAjcqwQI"
    "5KsECOyrBAj0qwQI/KsECASsBAgMrAQIFKwECBysBAgkrAQILKwECDSsBAg8rAQIRKwECEysBAhUrAQIXKwECGSsBAhsrAQIdKwE"
    "CHysBAiErAQIjKwECJSsBAicrAQIpKwECKysBAhsMZ/lAFCg4wIAVeEAEJPlFwAAKgJwoOEIQKDhAMCg4wQAXOENAAAqBTig4SM4"
    "oOGYAw7gDACg4QEwjOIDOKDhQ8ig4QwooOEgOI7ggzCg4SIIVOECAKDhsWCD4fX//4oBMIXiAzig4SMIV+FDWKDh6f//ivBBvege"
    "/y/h9DCf5d///+rwMJ/l3f//6uwwn+Xb///q6DCf5dn//+rkMJ/l1///6uAwn+XV///q3DCf5dP//+rYMJ/l0f//6tQwn+XP///q"
    "0DCf5c3//+rMMJ/ly///6sgwn+XJ///qxDCf5cf//+rAMJ/lxf//6rwwn+XD///quDCf5cH//+q0MJ/lv///6rAwn+W9///qrDCf"
    "5bv//+qoMJ/luf//6qQwn+W3///qoDCf5bX//+qcMJ/ls///6pgwn+Wx///qlDCf5a///+qQMJ/lrf//6owwn+Wr///qiDCf5an/"
    "/+qEMJ/lp///6oAwn+Wl///qfDCf5aP//+pMDQADUA0AA1QNAANYDQADXA0AA2ANAANkDQADaA0AA2wNAANwDQADdA0AA3gNAAN8"
    "DQADgA0AA4QNAAOIDQADjA0AA5ANAAOUDQADmA0AA5wNAAOgDQADpA0AA6gNAAOsDQADsA0AA7QNAAO4DQADvA0AA8ANAAPEDQAD"
    "yA0AAw3AoOEw2C3pAFig4QBAoOMlCFThBLBM4gEAADowaBvpHv8v4TgAAOsBMITiAzig4QUAU+FDSKDh+f//Ovb//+pCL6DjEEAt"
    "6UPPoOMB46DjgSOC4oJAoOOwQMLhDDCO4ABAoOOBw4ziQu+O4oQgoOOwIMzhARig4QAIoOGwQM7hsEDD4UEYoOEDwKDhIAig4bAw"
    "3OEAAFPh/P//OgEzoONCP4PiPwbg4wDgoOMBKKDhPwxA4rDgw+EDwKDhIhig4QIAQOKwINzhkEKD4AM1oOEjCFHh+v//ikI/oOND"
    "L6DjAROg4wDgoOOBM4PigSOC4kMfgeKw4MPhsODC4bDgzOGw4MHhEEC96B7/L+EBI6DjtDDS4QEAE+P8//8aASOg47Qw0uEBABPj"
    "/P//Ch7/L+HwQS3pQu+g40NPoOMBU6DjgjCg44HjjuKwMM7hBMCF4AAwoOOBQ4TiQl+F4oSAoOOwgMThsDDF4bAwzOGwMNzhAQBT"
    "4QJgoOENAACqADGf5QAgk+UAMJLlADAT4LAwzgGwMMQBsDDFAbAwzAE2AAAKAcOg40PPjOKwMNzhAQBT4fP//7oBw6DjQs+M4gCA"
    "oOOwgMzhPzbg47AQ3OE/PEPiAjBD4pPhguACJaDhIghW4RgAANqYMJ/lQk+g40NfoOMBc6DjAOCT5YFDhOKBU4XiQ3+H4gAwnuU/"
    "FuDjADAT4D8cQeKwMMQBAhBB4rAwxQGwMMwBsDDHARMAAAoBw6DjQs+M4rAg3OGRgoPgAzWg4SMIVuHu///KAROg4wEAoOFCP6Dj"
    "Qy+g4wDAoOOBM4PigSOC4kIfgeJDD4DisMDD4bDAwuGwwMHhsMDA4fBBvege/y/h0A0AAw3AoOEw2C3pBLBM4ljQTeJsQEviADCg"
    "4UgQn+UEAKDhVSCg4/9QA+KxBwDrbDBb5QAAU+MAAKDjBwAACgUAU+EBIIDiAUCE4gQAAAoAMNTlAABT4/8AAuL3//8aAACg4zBo"
    "G+ke/y/hcP0ECA3AoOHw3y3pBLBM4pjQTeICUKDhACDS5QEYoOEAAFLjAAig4Qgg2+UDoKDhQZig4Qww2+UEENvlAGCg40AIoOGs"
    "IAvlpAAL5agQC+WwMAvlBoCg4QZwoOEAIKADDgAACgYgoOEiOKDhAwDV5yjAS+KDQIzgxv//6wEwhuIDOKDhQ2ig4QYooOEiONXn"
    "/wAA4gAAU+O4B0Th8f//GgBgoOMiGKDhAQBW4bggC+U9AAAqCZig4aTgG+XAkAvlDuig4bAgG+XAMBvlvOAL5QKWoOEjCKDhBiCg"
    "4S5YoOEIyKDhLAhR4RQAAIoiyKDhKOBL4owwjuC6F1PhAABR4wEwhwIDOKABAYCgAUN4oAF0AAAKuDdT4QAAU+NCKKARDECgEUkA"
    "ABoBMIfiAzig4QLBoONDeKDhAIDg48zHoOGoEBvlrDAb5QAAUeMAAFMTFwAACijgS+KiN47gByig4SIogOC4F1PhgiKF4CwoguAJ"
    "EIHhgiCg4QMbgeO6EILhATCG4gM4oOFDaKDhuDAb5QYooOEjGKDhATCI4gM4oOEiCFHhQ4ig4cz//4rwbxvpHv8v4agQG+UAAFHj"
    "CgAACijgS+KiN47gByig4SIogOC4F1PhgiKF4CwoguAJEIHhgiCg4QEbgePj///qrBAb5QAAUeMKAAAKKOBL4qI3juAHKKDhIiiA"
    "4LgXU+GCIoXgLCiC4AkQgeGCIKDhAhuB49X//+oHOKDhKOBL4iM4gOCiF47guCdR4YMyheAsOIPggzCg4QkgguG6IIPhy///6gEw"
    "QuIDOKDhQyig4fv//xoBIILiAjig4SMIVOG0IAvlQ8ig4RMAAJoHKKDhwBAb5SI4oOEhOIPgvBAb5QggZuCDMqDhIeiD4AIQjOAB"
    "IIziATig4QIooOEjOI7gAMCg44MwoOEiCFThusCD4QEQgeJCyKDh9P//irTgG+UBMIfiDiig4QM4oOFCaKDhAICg40N4oOEGKKDh"
    "CMCg4ZP//+oNwKDh8N8t6QLgoOGUIZ/lBLBM4pJOjOAc0E3iLEOg4TVAS+WAIZ/lNVBb5QDA0uUAAFXjACCg4zYgS+UzIEvlLyBL"
    "5S4gS+UtIEvlLCBL5SsgS+U3IEvlNCBL5UwhnwUBGKDhA5Cg4UGooOGSHoMAAAig4aMyoAE4wEvlQAig4TDAS+UEYNvlCHDb5QyA"
    "2+U2MEsFBgAACoQyZOADMYTgCBGf5YMxTuCRw4LgoiKg4TYgS+U2wFvlAABc4/AgnwWSHoMAozGgATcwSwUJAAAK2CCf5ZJOg+Cj"
    "MqDhAzGD4AMxg+DIEJ/lAzFO4JFDguCiIaDhNyBL5bQgn+WSToPgozGg4QMxg+CDME7gAABV4zgwS+UE4KATBgAAGgAAXOMD4KAT"
    "AwAAGjcwW+UAAFPjAeCgAwLgoBMAIKDjKBBL4g4wgeAOAFLhAsCg4RAgQ+UKAAAqDjBs4ChAS+IDIITgERBS5QEwjOIMIITg/8AD"
    "4jAQgeIOAFzhCBBC5fT//zoKGKDhAAig4SAIoOEhGKDhMCBL4gkwoOHAAY3o3P7/6/BvG+ke/y/h001iENjOBAgfhetRzczMzDBA"
    "LelQMJ/l/xAB4gDgoOMAUJPlAECg4Q7AoOEBAqDhATCO4iwYoOEDOKDhASCA4EPooOGBEKDhDjig4bQQkeGCIKDhDwhT4wPAoOG1"
    "EILh8v//mjBAvege/y/h5A0AAzBALelQMJ/l/xAB4gDgoOMAUJPlAECg4Q7AoOEBAqDhATCO4iwYoOEDOKDhASCA4EPooOGBEKDh"
    "Djig4bQQkeGCIKDhDwhT4wPAoOG1EILh8v//mjBAvege/y/h6A0AAwTgLeU4MJ/lABCg4wDgk+UBwKDhATCB4gM4oOFDGKDhASig"
    "4aw3oOH/CFLjAsCg4bAgk+G+IIPh9f//mgTgneQe/y/h5A0AAzQwn+UAEKDjAMCT5QEAoOEBMIHiAzig4UMYoOEBKKDhoDeg4f8I"
    "UuMCAKDhACCg47wgg+H1//+aHv8v4eQNAAMBGKDhIRig4QESYeAYMJ/lAAig4QESoOEgGIHgAACT5YEQoOGwIIHhHv8v4cwNAAMw"
    "AJ/lABCg44EhoOGgwKDjARCB4gAwguCwwILhfwBR4wAgoOPwwKDjtCDD4bLAw+H0///aHv8v4YAGAAP/AADi/xAB4gEAUOEE4C3l"
    "CwAAqjDAn+WAIaDhoOCg4wEAgOIMMILgvOCC4QEAUOEAIKDj8OCg47Qgw+Gy4MPh9P//ugTgneQe/y/hgAYAAwLAoOGyMNDhsCDQ"
    "4f48A+L/LALisjDA4bAgwOEE4C3lAABR4wHgoOGwMNDhAuyBsrIQ0OEAAFzjAcyMsg4QgeEMMIPhsDDA4bIQwOEE4J3kHv8v4QIo"
    "oOFwQC3pImig4QDgoOMBGKDhBgBe4QBQoOFBGKDhEQAAKkgwn+UBKKDhAECT5aIFoOEOwKDhATCO4iwYoOEDOKDhASCA4EPooOGB"
    "EKDhDjig4bUQkeGCIKDhIwhW4QPAoOG0EILh8v//inBAvege/y/h8A0AAxBALelEMJ/lAACg43/PoOMA4JPlOECf5QPAjOIAEKDh"
    "ATCA4gM4oOFDCKDhACig4aE3oOEiCFzhAhCg4bQgk+G+IIPh9f//KhBAvege/y/h7A0AA4AGAAMAABDvHv8v4QAAEe8e/y/hAAAS"
    "7x7/L+EAABPvHv8v4QAAFO8e/y/hAAAV7x7/L+EAABbvHv8v4QAAF+8e/y/hAAAY7x7/L+H/EAHiAAig4YESoOEAwN3lIBiB4Awm"
    "guGBEKDhsyCB4R7/L+ENwKDhcNgt6QEzoOMAQKDj7yCg47Alw+EDYKDhBLBM4gBYoOEEMKDhQzyg4bQ1xuEAAKDjJRig4TX9/+sB"
    "IITi/0AC4gQ8oOEPBFPj9f//2nBoG+ke/y/hDcCg4XDYLekBM6DjAECg4+8goOOwJcPhA2Cg4QSwTOIAWKDhBDCg4UM8oOEQMGPi"
    "tDXG4QAAoOMlGKDhHv3/6wEghOL/QALiBDyg4Q8EU+P0///acGgb6R7/L+EAAKDjHv8v4Q3AoOHw2S3pVEGf5QzQTeIEsEziuCDU"
    "4bQw1OG2wNThsgDU4bDg1OGyEFThAsiM4AMIgOAsIZ/lLDGf5Q4YgeAAEIPlAACC5SAxn+UgIZ/lAuyM4gDgg+UAwILlDgEA6xCB"
    "n+VNAADrujDU4bAwyOGwMNjhAABT47RAVAH4IJ/l+DCf5fhwn+UAIIPlsEDIAQDAoOMAMJflDECg4wsAoOMBEKDj3CCf5QTAjeUA"
    "wI3lCECN5ZT9/+uwMNjhCQBT48Rgn5UNMKCTBwAAmgowQ+IDOKDhWQhT46xgn5WwQMaRAgAAmqBgn+ULMKDjsDDG4QBAoOOwANbh"
    "sCDY4QAwl+UMUKDjAhCg4wBAjeUwAI3pOP7/6wAwl+UPAKDjAhCg42ggn+UIUI3lAECN5QRAjeV1/f/rWBCf5Vgwn+VYIJ/lABCD"
    "5QBAwuWwQMbhsEDI4UQEAOu0AgDrMAMA6/gAAOv8///q4u8ECMAKAAPgCgADgAsAA8ALAAOACgADIAsAAwALAAPgCwADyP0ECJAL"
    "AAPU/QQI7u8ECGAGAAPoCgADDcCg4fDdLekC203iBLBM4gTQTeKQMp/lQR+g4wBAk+WBE4HigjCg4wYMoOMpzaDjMe2g4wrAjOIP"
    "4I7iASOg47AwweEFAIDiARug4309oOO4EMLhugDC4bzAwuG+4MLhADCC5UQyn+VEAp/lAECD5QwQoONl/v/rOAKf5Q0QoONi/v/r"
    "MAKf5Q4QoONf/v/rKAKf5Q8QoONc/v/rIAKf5QAQoONZ/v/rKi2g4xQCn+UAEKDjoPv/6wwCn+UBEKDjUv7/6/YuoOMAAp/lARCg"
    "45n7/+vwAZ/lAhCg40v+/+vgIKDj6AGf5QIQoOOS+//r1AGf5QMQoONE/v/r1AGf5QMQoOPgIKDji/v/68hxn+XIgZ/lAGCg4wBA"
    "oOOGUqDhBDCF4IMwoOG3IJPhBACg4QAwmOUBwKDjBhCg4QDAjeUO///rATCE4v9AA+IfAFTj8v//mgEwhuL/YAPiEwBW4+z//5qC"
    "rkviAICg4wSgSuIAAFjjaDGfFWgxnwUAcJMVZEGfFQBwkwVgQZ8FCsCg4QBgoOMAUKDjAOCg44UBoOEAEKDjATCA4AEggeKDMKDh"
    "/xAC4rQwk+EHAFHjsjDM4Pf//5oBMI7i/+AD4gMAXuPy//+aATCF4v9QA+IDAFXj7P//mgEwhuL/YAPiBwBW4+f//5oAYKDjAECg"
    "44ZSoOEEIIXgJBBL4oIwgeACO0PisCDT4QQAoOEHMKDhAsCg4wYQoOEAwI3l1P7/6wEwhOL/QAPiHwBU4/D//5oBMIbi/2AD4h8A"
    "VuPq//+aATCI4v+AA+IBAFjjxP//mkn+/+tMAJ/lABCg4wD+/+uAAJ/lABCg40AgoON6/v/rdDCf5UEuoOMNIILiARmg4xwQgeKw"
    "IMPhACCg47IQw+G0IMPhjv7/6/BtG+ke/y/hXA0AA+ALAAOe5AQIDv0ECC79BAhO/QQIPuUECF7lBAjezgQI/tMECJ7iBAi+4wQI"
    "/s4ECGQNAAN8DQADdA0AA37jBAhe4gQIvuQECIAGAAMNwKDhENgt6QE7oOMBI6DjBDCD4gAwguWAEJ/lBLBM4gAAkeXq/f/rAQCg"
    "453+/+tsMJ/lBhSg4wAAk+WC/v/rEPz/6woAoOOs/v/rAQCg4wAQoOPIIKDjE/z/6zIAoOOQ/v/rAECg4yAQoOP/AATiADCg4wEg"
    "oOEa+//rATCE4gM4oOEfCFPjQ0ig4fX//5r8+//rAQCg4xBoG+mX/v/qwAsAA4ALAAMNwKDhANgt6QSwTOLz+//rPDCf5QAg0+UB"
    "IILiACDD5QAg0+UsEJ/lPwAS4wAw0QUBMIMCADDBBYYAAOs+/v/rFDCf5QAAk+UEAADrAGgb6R7/L+E5DQADOA0AAwALAAPwRy3p"
    "6MGf5QAwnOUAIJPltDHQ4SAAEuMBEMMDAxCgEQAwnOUAoKDhACCT5bYx2uEQABLjsBDK4QEQwwMDEKARADCc5QAgk+W4MdrhQAAS"
    "47IQyuEBEMMDAxCgEQAwnOUAIJPlujHa4YAAEuO0EMrhARDDAwMQoBEAMJzlACCT5bwx2uEBABLjthDK4QEQwwMDEKARADCc5QAg"
    "k+W+MdrhAgAS47gQyuEBEMMDAxCgEQAwnOUAIJPlsDLa4QgAEuO6EMrhARDDAwMQoBEAMJzlACCT5bIy2uEEABLjsBHK4QEQwwMD"
    "EKARADCc5QAgk+WyEcrhtDLa4QIMEuMBEMMDAxCgEQAwnOUAIJPltjLa4QEMEuMBkMMDA5CgEQAwnOW8EMrhABCT5QAAk+UAwJPl"
    "AOCT5QCAk+UAQJPlAFCT5QBgk+UAcJPlACCT5aESoOEgAqDhLMOg4a7joOGkQKDhpVGg4SZhoOGndKDhIiSg4QEQIeIBACDiAcAs"
    "4gHgLuIBgCjiAUAk4gFQJeIBYCbiAXAn4gEgIuIBEAHiAQAA4gHADOIB4A7iAYAI4gFABOIBUAXiAWAG4gFwB+IBIALitiLK4b6Q"
    "yuG0EcrhtgHK4bjByuG64crhvIHK4b5ByuGwUsrhsmLK4bRyyuHwR73oHv8v4dANAAPwwZ/lADDc5QIAU+ME4C3lRwAAiuAxn+UA"
    "MNPlAwAT4zcAABo/MAPiMABT4wPxn5czAADq5MEECLzBBAi8wQQIvMEECLDBBAi8wQQIvMEECLzBBAjcwQQIvMEECLzBBAi8wQQI"
    "1MEECLzBBAi8wQQIvMEECMzBBAi8wQQIvMEECLzBBAjEwQQIvMEECLzBBAi8wQQIvMEECLzBBAi8wQQIvMEECLzBBAi8wQQIvMEE"
    "CLzBBAjMwQQIvMEECLzBBAi8wQQI1MEECLzBBAi8wQQIvMEECNzBBAi8wQQIvMEECLzBBAiwwQQIvMEECOTBBAi8wQQI5MEECCIg"
    "oOMBM6DjTCCD5QTgneQe/y/hqiCg4/n//+qIIKDj9///6mYgoOP1///qRCCg4/P//+oAIKDj8f//6sAwn+UAINPlAQAS4/8QAuII"
    "AAAasDCf5QAg0+UCAFLjATOgAwEzoBO6EcMBvhHDEbgRwwG8EcMRADDc5QYAU+Pi//+aAROg47ww0eEDMMPjvDDB4XDgn+W+INHh"
    "AADe5QMgwuMAMKDjAwBQ4wAwzOW+IMHhCgAACgIAUOPT//8avDDR4QMwg+O8MMHhviDR4QEgguO+IMHhAzCg4wAwzuXK///qvDDR"
    "4QIwg+O8MMHhviDR4QIwoOMDIILjviDB4fX//+o4DQADOQ0AA/wNAAMNwKDh8N8t6Twxn+UEsEzigNBN4gAIoOEAYJPlIAig4QAB"
    "gOAAkKDjAGOG4AmAoOGo4EviKDBL4n/AoOMBwFzigIBD5QEwg+L7//9aAHCg4wMAV+MH8Z+XBAAA6ijDBAjwwwQI+MMECADEBAgA"
    "oKDjAMCg4wFQoOMKQKDhBADW54AgoOMDEKDjAgAQ4QBQzhWiIKDhAgAQ4QAw3hUQMIMTADDOFQEQUeKiIKDhAeCO4vT//1oBwIzi"
    "BwBc4wJAhOLt///aAXCH4gMAV+Pf///aAkCJ4oQYoOEhGKDhqABL4kAgoOPf/P/rCTKg4VzAn+WEAaDhKDCD4kcuoOMBkIniDBCA"
    "4AE5g+MEQaDhCiCC4gkAWeO8IIDhtEDB4bIwweEgYIbiwf//2uz8/+vwbxvpHv8v4QGgoOPM///qEKCg48r//+oRoKDjyP//6sAK"
    "AAOABgADDcCg4fDfLekUMZ/lDNBN4gSwTOIAUNPl/yAA4gVRheCCaKDh/HCf5QIgheD4oJ/lAWeG4oIQoOEmaKDhAECg4/qQkeEA"
    "MJflBhCg4QEgguIGAKDjAECN5QRAjeUIQI3lp/v/68Qwn+UJGKDhACCT5SEYoOG4gJ/lgRFh4AAwl+UBIYLgCgCg4wYQoOEAQI3l"
    "BECN5QhAjeXe+v/rsBDY4QVQgeAFWKDhJVig4YEYoOGFAKDhAReB4vqQkOEAMJflASCF4g5goOMhGKDhBgCg4wBAjeVQAI3pifv/"
    "67AQ2OFIMJ/lCQig4QAgk+WBGKDhIAig4YABYOABF4HiACGC4AAwl+UhGKDhCgCg41AAjekAQI3lv/r/6wUAoOHwbxvpYf//6ugK"
    "AAPgCwAD8AsAA2AGAAOACgADDcCg4fDfLelIIZ/lBLBM4hDQTeIAMNLlPCGf5QMxg+D/kAPisBDS4QUwieIFAKDjAwBR4SwAC+UG"
    "AACqHCGf5ZIBg+AjMaDhAzGD4AEwY+D/MAPiLDAL5QShn+UEQKDjAFCg4wAwmuUEEKDhBQCg4fAgn+UAUI3lBFCN5QhQjeWW+v/r"
    "AjCE4v9AA+IPAFTj8///miwwG+UDAFXhBXCg4SQAACoFgKDhh1ig4QcgieC0AJ/lAVeF4gAAV+OCEKDhJVig4fBAkeEOYKADAGCg"
    "EwAwmuUBIILiBgCg4wUQoOEAgI3lBICN5QhgjeU2+//reDCf5QRIoOEAIJPlJEig4YRBZOAAMJrlCgCg4wQhguAFEKDhCGCN5QCA"
    "jeUEgI3lbvr/6wEwh+IsABvl/3AD4gAAV+Hb//86LDAb5QMwmeEBAAAa8G8b6R7/L+EJAKDh8G8b6Qb//+roCgADoAsAA83MzMzg"
    "CwAD3P0ECPALAANgBgADQROg4wEwoOMAIKDjATCD4gM4oOFVCFPjASDB5EM4oOH5//+aAROg40MvoOOwEIHisDCg4wMgguIAAKDj"
    "ATCD4gM4oOEjCFLhAQDB5EM4oOH5//8qAROg438w4OMAMMHlASCg4QEwoOMhEIHiADDB5TcwwuUnMMLlMTDC5R7/L+ENwKDh8N8t"
    "6ZQzn+UEsEziDNBN4gAQk+WwINHhAABS4wIAABq8wNHhAABc4xAAAApwI5/lADDS5QAAU+MBAAAa8G8b6R7/L+EBMEPiADDC5VQj"
    "n+UAAKDjsADC4Wr//+tIA5/lHBCg4x0goOPwbxvpx/v/6rIw0eEAAFPjAgAAGr4w0eEAAFPjDAAAChTjn+UcM5/lAADe5bAQ0+EA"
    "IYDgBSCC4gIAUeHk///a+DKf5QEggOKwwMPhACDO5eb//+q0wNHhAABc4yUAAArYEp/lsDDR4QAAU+MBMEMSsDDBEQAA0RUBAIAS"
    "GwAAGrQyn+UAINPluDKf5QIhguCwENPhBSCC4gIAUeGcIp+lBDCgo7AwwqEHAACqmCKf5ZIBg+AjMaDhAzGD4AEwY+B4Ep/lATBD"
    "4rAwweEAAKDj5P7/62Qyn+WwINPhYAKf5QIioOEdIILiHBCg48P//+r/AADi9f//6rYA0eEAAFDjJwAACjAyn+UAINPlNDKf5QIh"
    "guCwANPhBSCC4gIAUOELAAC6FAKf5bAw0OEDAFPjATCDkrAwwJEAANCV/BGfhQQAoIOwwMGB4P//igEAQOLm///q8DGf5ZMQguAi"
    "IaDhAiGC4AAAYuDQEZ/lADig4bAg0eEjOKDhATBD4gMAUuG4IZ+lAQBAov8AAKKwwMKhzv//qgEwguKwMMHhAADR5en//+q4MNHh"
    "AABT4wIAABqwMdHhAABT44X//wp4IZ/lADDS5XQhn+WwENLhAzGD4HiBn+UDEIHgsBDI4bAw2OFsEZ/lgzCg4bEgk+FkkZ/lAGCg"
    "4YKhYuAAUKDhPCGf5bAQ0uGwINjhgRig4QEAFuMBF4HiAECgEw1AoAMAMJnlIRig4QEgguIGAKDjAFCN5QRQjeUIQI3lS/r/6/ww"
    "n+UUAZ/lsBDT4QAgkOWBGKDhAReB4gpxoOEAMJnlIRig4QcgguAKAKDjCECN5QBQjeUEUI3lgfn/64YZoOEGF4HiIRig4QUAoOHP"
    "+P/rATCG4v9gA+ICAFbj1///mrAw2OGsEJ/lgzCg4bEgk+GoMJ/liACf5QAQk+WwUMDhgiFi4AIRgeATMNHl/wBT4wQAAAoFEKDh"
    "AQCg47v4/+tXAADrO///6nQwn+UCFKDjAACT5Vf7/+tgEJ/lADCR5QMwh+C4AdPhuiHT4QIUoOO9HIHiAgiA4EAQgeJN+//rBRCg"
    "4QEAoOOo+P/r+f7/6wIEoOMQ/y/hJv//6gALAAPoCgADgAoAA4AGAAOgCwADzczMzJALAAPwCwAD4AsAA2AGAAPgCgADBOAt5VU8"
    "oOMAwKDjAiOg46owg+KwMMLhDhSg4wDAguX/AADisMDC4VUw4ONVIKDjDOCg4QAwweUAIMHlAQDB5QDAweUAwMHlBOCd5B7/L+H/"
    "AADiVSzg4wIToOMAAqDhqiBC4gAwoOOwIMHhADCB5bAAweEe/y/hcEAt6Uwwn+VM4J/lAFCT5QAwoONEYJ/lsDDO4Q5AoOGwANTh"
    "gCFg4AIR1ecAAFHjAcCA4oAgoOEEAAAKsMDE4bAw3uF3AFPjtgCC4fP//5pwQL3oHv8v4WAGAAOgCwAD8AsAAw3AoOHw2S3pRCGf"
    "5bAw0uFAIZ/lgzCg4fIQk+E4cZ/lAYig4ShIoOEAMJflhEFk4ARBoOEDMITgFCDT5RVg0+UDIELiBLBM4gQAoOP/UALis///6wYA"
    "oOHF///rDACg46///+sAMJflA0CE4BMg1OW2MdThAyiC4OAwn+UAAFXjACCD5dgAnwUCO6ADLgAACgEAVePIAJ8FATugAyoAAAoC"
    "AFXjuACfBQY8oAMmAAAKAwBV46gAnwUAMKADIgAACgQAVeOYAJ8VHQAACgcAVuOwMNCRBjKDkbAwwJEAEJflKDig4YMxY+ADEYHg"
    "ECDR5bAw0OEBIILiAjCD4bAwwOG2IdHhXECf5QAAUuMCM6DjADCE5boh0QG4MdEBAjiDAAIzgwIAMIQFaf7/6wAAlOU0MJ/lD+Cg"
    "4RP/L+HwaRvpHv8v4RgAn+UBOqDjsDDA4d3//+qQCwAD8AsAA2AGAAOgCgAD4AwAA2ALAAMQBgADHv8v4QEzoOOCP4PiACCg47AQ"
    "0+EcAJ/lsCDD4RgQn+UBMILiAhGA5/8gA+INAFLj+v//mh7/L+EADQADWM0ECAHDoOMCzIzisCDc4f8AAOIBMKDjEyCC4Wwwn+UC"
    "AFDjsCDM4QARg+cBI6DjBgAAigEAUOMRAAAKCwAAygAAUOO0MNIBCDCDAwUAAAoBI6Djgi+C4rAw0uEBMIPjsDDC4R7/L+G0MMLh"
    "9///6gIAUOP1//8atDDS4SAwg+P4///qtDDS4RAwg+P1///qAA0AAwEToOMCHIHisCDR4f88oOP/AADi/jCD4hMgAuACAFDjsCDB"
    "4QEjoOMGAACKAQBQ4w8AAAoJAADKAABQ47Qw0gEIMMMDAwAACjAgn+UwMJ/lACGD5x7/L+G0MMLh+f//6gIAUOP3//8atDDS4SAw"
    "w+P4///qtDDS4RAww+P1///qWM0ECAANAAMBIELiAQBy4wDAoOEe/y8BATDR5AEgQuIBAHLjATDM5B7/LwH5///qAAAAAAAAH3wA"
    "AP9/10qAAad64mTjDKIANAFZA999HWAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQACAAMABAAFAAUABQAFAAUABQAGAAcAAgQBBAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAACAAJAAoACwAMAA0ADgAPABAAEQASABMAEwATABMAEwATABMAEwASBBEEEAQPBA4EDQQMBAsECgQJ"
    "BAgEAAAAABQAFQAWABcAGAAZABoAGwAcAB0AHgAfABMAEwATABMAEwATAB8EHgQdBCAAIQAiACMAJAAlABYEFQQUBAAAAAAmACcA"
    "KAApACoAKwArBCoELAAtAC4ALwAwADEAMgAzADQAMAA1ADYANwArBCoELAAqACsAOAA5ACcEJgQAAAAAOgA7ADwALAgqCCsIKwwq"
    "DCwIKggrCCsMKgwsCCoIKwgrDCoMLAgqCCsIKwwqDCwIKggrCCsMPQA+ADoEAAAAAD8AQABBAEIAQQRDAEMEQQBCAEEEQwBDBEEA"
    "QgBBBEMAQwRBAEIAQQRDAEMEQQBCAEEEQwBDBEEARAA/BAAAAAA/AEUARgBHAEYESABIBEYARwBGBEgASARGAEcARgRIAEgERgBH"
    "AEYESABIBEYARwBGBEgASARGAEkAPwQAAAAAPwBKAEEIQghBDEMIQwxBCEIIQQxDCEMMQQhCCEEMQwhDDEEIQghBDEMIQwxBCEII"
    "QQxDCEMMQQhLAD8EAAAAAD8ATAAqBCwAKgArACsEKgQsACoAKwArBCoELAAqACsAKwQqBCwAKgArACsEKgQsACoAKwArBCoETQA/"
    "BAAAAAA/AE4AKgwsCCoIKwgrDCoMLAgqCCsIKwwqDCwIKggrCCsMKgwsCCoIKwgrDCoMLAgqCCsIKwwqDE8APwQAAAAAPwBQAEEA"
    "QgBBBEMAQwRBAEIAQQRDAEMEQQBCAEEEQwBDBEEAQgBBBEMAQwRBAEIAQQRDAEMEQQBRAD8EAAAAAD8ARQBGAEcARgRIAEgERgBH"
    "AEYESABIBEYARwBGBEgASARGAEcARgRIAEgERgBHAEYESABIBEYASQA/BAAAAAA/AEoAQQhCCEEMQwhDDEEIQghBDEMIQwxBCEII"
    "QQxDCEMMQQhCCEEMQwhDDEEIQghBDEMIQwxBCEsAPwQAAAAAPwBSAFMAVABTBFUAVQRTAFQAUwRVAFUEUwBUAFMEVQBVBFMAVABT"
    "BFUAVQRTAFQAUwRVAFUEUwBWAD8EAAAAAFcAWABZAFkAWQBaAFoAWgBbAFkAWQBZAFoAWgBbBFoEWgRaBFkEWQRZBFsEWgRaBFoE"
    "WQRZBFkEWARXBAAAAABcAF0AXgBfAF8AXwBfAF8AXwBfAF8AXwBfAF8AXwBfAF8AXwBfAF8AXwBfAF8AXwBfAF8AXwBeBF0EXAQA"
    "AAAAYABhAGIAYwBkAGQAZABkAGQAZABkAGQAZABkAGQAZABkAGQAZABkAGQAZABkAGQAZABkAGMEYgRhBGAEAAAAAGUAZgBnAGgA"
    "aQBqAGoAagBqAGoAagBqAGoAagBqBGoEagRqBGoEagRqBGoEagRqBGoEaQRoBGcEZgRlBAAAAABrAGwAbQBuAG8AcABwAHAAcQBx"
    "AHEAcgBwAHAAcQRwBHAEcARyBHEEcQRxBHAEcARwBG8EbgRtBGwEawQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAQAAAQgQARERgAAAAAAAAAEQAAEYUAEYV4EYV4V4V4V4d3JYURUocRoRERERF1V1qqh5WalxWIhYEYoain"
    "cRkRERmRERERmCFhERERESIqqqKampeagoKBgqiop6gRERERZmZmZlZVVVURERERKiqqopeal5qBgoGCp6inqBERERFmZmZmVVVV"
    "VREREREqKqoil5qXmoGCgYKnqKeoEREREWZmZmZVVVVlERERESqqdVeap1l4glGIUaiHGoEREZEXEREZkRYSiREAAAAAAAAAAAAA"
    "AAAAAAARAAAQqQAAoWYAEGlWABBqJQAAAAAAAAAAAAAAAAEAEREZESoapigZKWY5GJhlEooRAAAAAAAAAAAAAAAAAAAAABEREREX"
    "lxkXeqgSKYKKKXoAAAAAAAAAAAAAAAAAEREREaGioil6qXl6ESgYGHiKegAAAAAAAAAAAAAAABERERGioiIqqXmZmigYiIKKeqqo"
    "AAAAAAAAAAAAAAAAEREREaqiKiqXmpeagYKBgqeop6gAAAAAAAAAAAAAAAARERERqqIqmpealyeBgoGap6inEgAAAAAAAAAQAAAQ"
    "IRERIaoqeqKJGamTmHiYihqHGqKnEamih6kqqqKaoniXGZKHgYGSqqiBGnqhp6EREqJ6KqkRcap5iaF1GiNzFxI3inV6eKJ4p1kp"
    "gpGCmCqiqqJadZGZEWGYGWFmmRlmVpkSZlWXGWZVhRFmVakaZlWHEWZVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVUA"
    "EGlWAAChZgAAgSkAACETABCaqAAQEoEAEJoYABAhGWaJJ4GmOal4mamKijgqhhqXapaioogZmYiqkpJ4gZqZWlinhih3KRiHI4IR"
    "GJEYEYkoEYGRGnglqRFXd6mBd4hxVVKlEXh1VxGBhYcRWBiBV3KBV3UXeIWIVxeIdReIhCWlVVVXN3d3V5iIeIWBEYGBiIiIiHiI"
    "iIh4iIiISIiIWlVaVXN3c3eJeIl4GIEYgYiISIiHiHiIh4h4iISISIhaVVpac3dzN4l4iZcYgRiBiISISIiHiHiIh4h4iISISCVX"
    "haKneTkieCqiGIGBGIGIiIQYiIiHiIiIh4iIiISIGZKHgZqieJepKqqiF6mih3ERERh4eBGBeIh3F0iIiHc3inV6I3MXEomhdRoR"
    "cap5UocRoXclhRGFeFeHEYV4V5kSZlWZGWZVmBlmVZEZZlURGGFWGZERYXGZIWEYkRERVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV"
    "VVVVVVZVVVUYeCmReYcqqSqqopp4KppxgRERFxgRh3hxd4h4d4SISCpYdVIik5d6gSqihxiBGBiBiIiIiIiHiIiIh4iIiISIpaVV"
    "pXM3dzd5mIeYGIEYgYiIiIR4iIiHeIiIh0iIiIRVpVWldzd3N4eYh5gYgRiBiIiIiIh4iIiIeIiIiEiIiFVVWlJ3d3N1h4iJdRgR"
    "GFiIiIgYh4h4iIeIeIiEiEiIWiVVF3VXhxF4WBgRGIGFEXUYJ3VYh3FXiHF1iIiIcVcAAHEqAAARhwAAcagAAJGKAACRkgAAcSEA"
    "ABGpAABxcooXGZklGJiqeBOoEXWSEoGXgoF3KhhxdYYRUYcYESiHkRqIdZmZgReaknGBkRmIiBgRhIiBgUR3dYGEiBeIh4gXiEdE"
    "iIiIhHiIiId4iIiHSIiIhER0R0RIiIiEeIiIh3dEdEeISIiIiHiIiIh4iIiISIiId0R0R4hIiIiIeIiIRHdEdISISIiHiHiIh4h4"
    "iISISIhEd0R0hIhIiIeIeIhEdEdESIiIhHiIiId4iIiHSIiIhER0R0RIiIiEeIiIh3cRhXiIdxGFiHh3EYh4iHeISIiId0R0R4hI"
    "iIiIeIiIFRgREYelqKd1h4WBEZWal4hRWqpEGBERhIiIiIeIeIgRERERqKinqIKCgYKampeaIiqqohERERGIiIiIiIeIeBERERGn"
    "qKeogYKBgpeal5oqKqqiEREREYiIiIiIiIeIEREREaeop6iBgoGCl5qXmioqqqIRERERiIiIiHiIiIcRERERp6inqIGCgYKXmpea"
    "KiqqohERERGIiIiIiHiIiBERERGnqKeogYKBgpeal5oqKqqiEREREYiIiIiHiHiIEREREaeop6iBgoGCl5qXmioqqqIRERERiIiI"
    "iIiHiHgRERERp6inqIGCgYKXmpeaKiqqIhERERGIiIiIeIiIhxERgVGoh1p4glF4V5qnWREqqhWIERGBR4iIiIiIeIiIh1gRd1gR"
    "d4gRd3iId4h4iISISIhEd0R0hIhIiIeIeIhHRIhxiISIiIiHiHiIh4h4iISISEdEd0SIhIhIiIeIeFeIoRlxGJmZGBcpqYiIkRmI"
    "iBGBdEcYGIiIFFeIiIdxAACRigAAIRcAAKiBABB6cQAQenEAEJKoABCqggAQkqgREVdYFxF1eIWBcnd1WBcVUoWBF3UYeIF1EVdB"
    "WogVSBeIh4iBiISIcUdEd4iIhIiIiIeIiIiHiIiIhIh0R0R3iIiHcYiIhBh0R0QXiIiEiIiIh4iIiIeIiIiEiHRHRHeFdRERh1cR"
    "cXcnGFhRcYVXcRhYJRiHgVcYdRFXhFGIpQAQenEAEJKoABCqggAQkqgAEHJxABCaqAAQooIAEJKodVcXSFKFgXh1EYF4dRiISDoZ"
    "eER1EYhIdYGIeHUYiHiIiISIiIiHiIiIh4iIiISIdEdEd4iIhIiIiIeIiIiHiEiIiIR4iIiHeIiIh0iIiIREdEdESIiIhHiIiId4"
    "iIiHhIhIiIeIeIiHiHiIhIhIiER3RHSEiEiIh4h4iIeIeIhIcXVXeBhYJXgYEVdIiIFXRISRo0iIEVd4iBhXeIiBVzoZiEh1EXhE"
    "dYGISHUYiHg6GYh4dRGISHWBeER1GIhIiIiEiHRHRHeIiISIiIiHiIiIh4iIiISIdEdEd4iIhIhIiIiERHRHREiIiIR4iIiHeIiI"
    "h0iIiIREdEdESIiIhISISIhEd0R0hIhIiIeIeIiHiHiIhIhIiER3RHSEiEiISIiRo0SEEVdIiBhXeIiBV3iIkaNIiBFXRIQYV0iI"
    "gVc6GYh4dRGIeHWBiEh1GHhEOhmISHURiHh1gYh4dRiISHiIkaN4iBFXSIgYV0SEgVdIiJGjeIgRV3iIGFdIiIFXOhl4RHURiEh1"
    "gYh4dRiIeDoZiEh1EXhEdYGISHUYiHhEhJGjSIgRV3iIGFd4iIFXSIiRo0SEEVdIiBhXeIiBVzoZiHh1EYhIdYF4RHUYiEg6GYh4"
    "dRGIeHWBiEh1GHhEeIiRo0iIEVdEhBhXSIiBV3iIkaN4iBFXSIgYV0SEgVc6GYhIdRGIeHWBiHh1GIhIOhl4RHURiEh1gYh4dRiI"
    "eEiIkaN4iBFXeIgYV0iIgVdEhJGjSIgRV3iIGFd4iIFXOhl4RHURiEh1gYh4dRiIeDoZiEh1EXhEdRGBSHWYERF0R0R3iIiEiIiI"
    "h4iIiIeIiIiEiHRHRHeIiISIEREREUR0R0RIiIiEeIiIh3iIiIdIiIiERHRHREiIiIQRERERRHdEdISISIiHiHiIh4h4iISISIhE"
    "d0R0hIhIiBERERFEhJGjSIgRV3iIGFd4iIFXSIiRo0SEEVdIGBFXERGJVwAQenEAEJKoABCqggAQkqgAEHJxABCSqAAQqoIAEJKo"
    "OhkqKnURmJpVgYeCdVinqDoZVVV1GHV3dVGBeFKFGIGqoioql5qXmoGCgYKnqKeoWlVaVXN3c3eJeIl4GIEYgSoqqqKXmpeagYKB"
    "gqeop6haVVpVc3dzd4l4iXgYgRiBKioqKpeal5qBgoGCp6inqFpVWlVzd3N3iXiJeBiBGIEAEHpxABCSqAAQqoIAEJKoABB6cQAQ"
    "enEAAKiBAAAhF3VXF4FaiBUYdRFXEXUYeIFShYEXdVgXFYWBcncXEXV4GIEYgREREREREREREREREREREREYEREREREREYEREREY"
    "gRiBEREREREREREREREREREREREREREREREREREREQAAkYoAAHFyAAARqQAAcSEAAJGSAACRigAAcagAABGHERFXWBgRKIeGEVGH"
    "KhhxdZeCgXd1khKBeBOoESUYmKoXERERFxgREXURERGBgRERGBEREZEZiBGaknGBmZmBFxERERERERERERERERERERERERERERER"
    "ERERERGIEREREREREREREREREREREREREREREREREREREREREREREREAAHEqABAhGQAQmhgAEBKBABCaqAAAIRMAAIEpAAChZooX"
    "GZl4gZqZiKqSkqKIGZmXapaiOCqGGpmpioqmOal4kRqIdamBd4ipEVd3kRp4JYkoEYEYkRgRhyOCESh3KRgXiBERdReIEYhXF4h1"
    "F3iFV3KBVxFYGIERgYWHEXh1VxEREREREREREREREYgRERGBiBERhYERgVeYiHhXN3d3EREREREREREREREREREREREREREYgRiB"
    "iXiJeHN3c3cAEGlWABBqJQAQaVYAAKFmAAAQqQAAABEAAAAAAAAAAGaJJ4FlEooRZjkYmKYoGSkZESoaAQAREQAAAAAAAAAAWlin"
    "hoKKKXp6qBIpF5cZFxEREREAAAAAAAAAAAAAAABxVVKlGHiKenoRKBgpeql5EaGiogAREREAAAAAAAAAACWlVVWKeqqoKBiIgql5"
    "mZqioiIqEREREQAAAAAAAAAAWlVaVaeop6iBgoGCl5qXmqqiKioRERERAAAAAAAAAABaVVpVp6inqIGCgYKXmpeaKiqqohEREREA"
    "AAAAAAAAAFpVWlWnqKeogYKBgpeal5oqKioqEREREQAAAAAAAAAAAAABAAEEAAAAAAEAAQQAAAIAAwADBAIEAgADAAMEAgQEAAUA"
    "BQQEBAQABQAFBAQEAAAGAAYEAAAAAAYABgQAALu7u7u7u7u7u7u7u7u7u7u7u7u7u7u7u7u7u7u7u7u7u7u7y7u7u8y7u8vMu7vM"
    "zLvLzMy7zLu7y7vMzLzLzMy7u7vLu7u7zLu7y7y7u8y8u8vMvLvMzMvLzMzLzMzMy8u7zMzLu7u7zLvMvMzLu8vMy8vLzMu7y8zL"
    "u7zMy8y8zMzMy8vMzMu7zMy8u8vMvLu7zLy7u8vMu7u7zLu7u8vMu8zMzLu7u8y7u7vMy7u8zLvMy8u8u7vLzLu8vMy8u8y7zMzL"
    "zLu7u8zMzLvLzMy7u8zMu7vLzLu7u8y7u7vLAAABAAEEAAAAAAEAAQQAAAIAAwADBAIEAgADAAMEAgQEAAUABQQEBAQABQAFBAQE"
    "AAAGAAYEAAAAAAYABgQAAMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMvMzMzLvMzLy7zMy7u8y8u7vMu8zMvMy7"
    "u8u8u7vMzMy8zMzMu8zMvMvMzLvLzLy7y8y7u7y8u7u8u7u7vLzMu7u8zMzMu8y7y7u8zLy7vLy8u7zMvLu8zMu7vLvLu7u7vLy7"
    "u7zMu7vLzLy7y8zMu8vMzLy7zMzMu8zMzLy7zLu7u8zMzLvMzMy7vMzLu8y7vLzLzMy8u8zLy7vLzLvMu7u8u8zMzLu7u8y8u7vM"
    "zLu7zMy8u8zMzLvMzMy8H3z/fwAA330dYDQBWQMAAAAAAAAAAAAAAAAAAAAAAAAAISIBECImERBSJiEhJSUiIVJiQiEiUjJSJmUl"
    "IiIlZQAAAAARAQAAIhIBAEQkEgBEMyQBMxMhAUQTQRJCNEMSUiZWZiIiJSUhUlJCISViQiEiJSIQUiYhECImEQAhIgFCNEQSRDRE"
    "EkQ0JAFEQyQBMyQSACISAQARAQAAAAAAAB98AAD/fwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAIiIAIAIgAiACIAIgIiICIAIgAiACIAIgAiACAAAAACAiIgAgAiACIAIgAiAiIgAgAiACIAIgAiAiIgAA"
    "AAAAACAiAgAiAAAgAgAAIAIAACACAAAAIgAAACAiAgAAAAAgIgIAIAIiACACIAIgAiACIAIgAiACIgAgIgIAAAAAACAiIgIgAgAA"
    "IAIAACAiAgAgAgAAIAIAACAiIgIAAAAAICIiAiACAAAgAgAAICICACACAAAgAgAAIAIAAAAAAAAAIiIAIAIgAiACAAAgAiICIAIg"
    "AiACIAIAIiICAAAAACACIAIgAiACIAIgAiAiIgIgAiACIAIgAiACIAIAAAAAACIiAAAgAgAAIAIAACACAAAgAgAAIAIAACIiAAAA"
    "AAAAACACAAAgAgAAIAIAACACAAAgAiACIAIAIiIAAAAAACIAIAIiACIAIiACACIiAAAiIAIAIgAiACIAIAIAAAAAIAIAACACAAAg"
    "AgAAIAIAACACAAAgAgAAICIiAgAAAAAiACACIgIiAiIiIgIiICACIgAgAiIAIAIiACACAAAAACIAIAIiAiACIiIgAiIgIgIiACIC"
    "IgAgAiIAIAIAAAAAACIiACACIAIgAiACIAIgAiACIAIgAiACACIiAAAAAAAgIiIAIAIgAiACIAIgIiIAIAIAACACAAAgAgAAAAAA"
    "ACAiAgAiACIAIgAiACIAIgAiIiIAIiAiACAiIAIAAAAAICIiACACIAIgAiACICIiACACIgAgAiACIAIgAgAAAAAAIiIAIAIgAiAi"
    "AAAAIiIAAAAiAiACIAIAIiIAAAAAACAiIgIAIAIAACACAAAgAgAAIAIAACACAAAgAgAAAAAAIAIgAiACIAIgAiACIAIgAiACIAIg"
    "AiACACIiAAAAAAAgAiACIAIgAiACIAIgAiACACIiAAAiIgAAIAIAAAAAACIAIAIiACACIgAgAiIgIAIiIiICIgIiAiIAIAIAAAAA"
    "IgAAIiACIAIAIiIAACACAAAiIgAgAiACIgAAIgAAAAAiAAAiIAIgAgAiIgAAIAIAACACAAAgAgAAIAIAAAAAACIiIgIAACIAACAC"
    "AAAiAAAgAgAAIgAAACIiIgIAAAAAAAAAAAAAAAAAIiIAAAAgAgAiIgIgAiACACIiAgAAAAAgAgAAIAIAACAiIgAgAiACIAIgAiAC"
    "IAIgIiIAAAAAAAAAAAAAAAAAACIiACACAAAgAgAAIAIAAAAiIgAAAAAAAAAgAgAAIAIAIiICIAIgAiACIAIgAiACACIiAgAAAAAA"
    "AAAAAAAAAAAiIgAgAiACICIiAiACAAAAIiIAAAAAAAAgIgAAIgAAICIiAAAiAAAAIgAAACIAAAAiAAAAAAAAAAAAAAAAAAAAIiIC"
    "IAIgAiACIAIAIiICAAAgAgAiIgAgAgAAIAIAACAiIgAgAiACIAIgAiACIAIgAiACAAAAAAAgAgAAAAAAACACAAAgAgAAIAIAACAC"
    "AAAAIgAAAAAAAAAiAAAAAAAAACIAAAAiAAAAIgAAACIAAAAiACAiAgAgAgAAIAIAACACIAIgAiIAICICACACIgAgAiACAAAAAAAg"
    "AgAAIAIAACACAAAgAgAAIAIAACACAAAgAgAAAAAAAAAAAAAAAAAiAiIAIiIiAiIgIAIiACACIgAgAgAAAAAAAAAAAAAAACAiIgAg"
    "AiACIAIgAiACIAIgAiACAAAAAAAAAAAAAAAAACIiACACIAIgAiACIAIgAgAiIgAAAAAAAAAAAAAAAAAgIiIAIAIgAiACIAIgIiIA"
    "IAIAACACAAAAAAAAAAAAAAAiIgIgAiACIAIgAgAiIgIAACACAAAgAgAAAAAAAAAAIAIiACAiIAIgAgAAIAIAACACAAAAAAAAAAAA"
    "AAAAAAAAIiIAIAIAAAAiIgAAACACICIiAAAAAAAAIgAAACIAACAiIgAAIgAAACIAAAAiAAAAICIAAAAAAAAAAAAAAAAAIAIgAiAC"
    "IAIgAiACIAIgAgAiIgIAAAAAAAAAAAAAAAAgAiACIAIgAiACIAIAIiIAACACAAAAAAAAAAAAAAAAACIAIAIiACACIiAgAiIiIgIg"
    "AiIAAAAAAAAAAAAAAAAAIgAgAiACIgAAIgIAIAIiACIAIAIAAAAAAAAAAAAAAAAgAiACIAIgAiACIAIAIiIAACACAAAiAAAAAAAA"
    "AAAAACAiIgIAACIAACACAAAiAAAgIiICAAAAAAAiIgAgAiACIAIiAiAiIgIgIiACIAIgAgAiIgAAAAAAACACAAAiAgAgIgIAACAC"
    "AAAgAgAAIAIAACACAAAAAAAAIiIAIAIgAgAAIAIAACIAACACAAAiAAAgIiICAAAAAAAiIgAgAiACAAAgAgAgIgAAACACIAIgAgAi"
    "IgAAAAAAACAiAAAiIgAgAiIAIgAiACIiIgIAACIAAAAiAAAAAAAgIiICIAIAACAiIgAAACACAAAgAiACIAIAIiIAAAAAAAAgIgAA"
    "IgAAIAIAACAiIgAgAiACIAIgAgAiIgAAAAAAICIiAgAAIAIAACACAAAiAAAgAgAAIAIAACACAAAAAAAAIiIAIAIgAiACIAIAIiIA"
    "IAIgAiACIAIAIiIAAAAAAAAiIgAgAiACIAIgAgAiIgIAACACAAAiAAAiAgAAAAAAACACAAAgAgAAIAIAACACAAAgAgAAAAAAACAC"
    "AAAAAAAgAiIAIAIiAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACACIgAgAiIAIiIiAiACIgAiIiICIAIiACACIgAAAAAAACACAAAi"
    "IgIgAgAAACIiAAAAIAIgIiIAACACAAAAAAAAAAAAIAIgAgICIgAiIAIAACIgAiACAgIiACIAAAAAAAAiAgAgAiIAIAICACAiIAIi"
    "ICIAIgAiAiAiAiIAAAAAACACAAAgAgAAIgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACIAACACAAAiAAAAIgAAACIAAAAgAgAAACIA"
    "AAAAAAAiAAAAIAIAAAAiAAAAIgAAACIAACACAAAiAAAAAAAAAAAAACACIAIAIiIAIiIiIgAiIgAgAiACAAAAAAAAAAAAAAAAACAC"
    "AAAgAgAgIiICACACAAAgAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACACAAAgAgAAIgAAAAAAAAAAAAAAAAAAICIiAgAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgAgAAIAIAAAAAAAAAACIAACACAAAiAAAgAgAAIgAAIAIAACIAAAAA"
    "AAAAAAAAAAAgAgAAIAIAAAAAAAAAAAAAIAIAACACAAAAAAAAAAAAACACAAAgAgAAAAAAAAAAAAAgAgAAIAIAACIAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAACAiIgIAAAAAAAAAAAAAAAAgIiICAAAAACAiIgIAAAAAAAAAAAAAAAAAAAAAIAIAAAAgAgAAACACACAC"
    "ACACAAAAAAAAAAAAAAAiIgAgAiACAAAgAgAAIgAAIAIAAAAAAAAgAgAAAAAAAwAmFQUIJhUFCAwABQgDAEdCQS1Ob3RlcGFkAAAA"
    "AAAAAAAHAwAAAMAAAABwaWFubwAAAAAAAAAAAAAAAAAACQMAgAEAAAMAQmlvc19EdW1wZXIAAAAAAAAAAAkDAAACAAAEAAAgICAg"
    "ICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAA"
    "AAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAg"
    "IAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAg"
    "ICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAA"
    "AAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAg"
    "AAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAg"
    "ICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAA"
    "AAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAA"
    "AAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAg"
    "ICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAA"
    "AAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAA"
    "AAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAg"
    "ICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAA"
    "ACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAA"
    "AAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAg"
    "ICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAA"
    "ICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAA"
    "AAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAg"
    "ICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAg"
    "ICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAA"
    "AAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAg"
    "ICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAg"
    "ICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAA"
    "AAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAg"
    "ICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAg"
    "ICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAA"
    "AAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAg"
    "ICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAg"
    "ICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAA"
    "AAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAg"
    "IAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAg"
    "ICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAA"
    "AAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAg"
    "AAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAg"
    "ICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAA"
    "AAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAA"
    "AAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAg"
    "ICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAA"
    "AAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAA"
    "AAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAg"
    "ICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAAAAAAAAAAAAAA"
    "ACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAgICAgICAgAAAA"
    "AAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAAAgICAgICAgICAgICAgICAAAAAAAAAAAAAAAAAAICAgICAgICAg"
    "ICAgICAgAAAAAAAAAAAAAAAAACAgICAgICAgICAgICAgIAAAAAAAAAAAAAAAAB98AAAfAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAH3wAAOF/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAffAAAtDMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIEFCQ0RF"
    "RkdISUpLTE1OT1BRUlNUVVZXWFlaYWJjZGVmZ2hpamtsbW5vcHFyc3R1dnd4eXowMTIzNDU2Nzg5ISAjJCUmICgpKissLS4vOjtf"
    "PT4/AAAAAEdCQSBNZW51AAAAAGluIDEAAAAAICAgICAgICAgICAgICAgICAgICAgICAgICAgICAA//8BI6DjADKy5bgQ0uEAAE/h"
    "B0At6QEAoOO4AMLhIxgD4OTAn+UBABHiJQAAGgTAjOICABHiIgAAGgTAjOIEABHiHwAAGgTAjOIIABHiHAAAGgTAjOIQABHiGQAA"
    "GgTAjOIgABHiFgAAGgTAjOJAABHiEwAAGgTAjOKAABHiEAAAGgTAjOIBDBHiDQAAGgTAjOICDBHiCgAAGgTAjOIBCxHiBwAAGgTA"
    "jOICCxHiBAAAGgTAjOIBChHiAQAAGgTAjOICChHisgDC4QAwD+HfMMPjHzCD4wPwKeEAAJzlAEAt6QDgj+IQ/y/hAEC96AAwD+Hf"
    "MMPjkjCD4wPwKeEHQL3ouBDC4QDwaeEe/y/hAA0AA1U84OMCE6DjqjBD4rAwweEcIJ/lHDCf5QDAkuWwINPhAMCB5bAgweEAMKDh"
    "APCg4R7/L+GgCgAD4AwAAwAAAAYAQAAGAIAABgDAAAYAAAAGAAgABgAQAAYAGAAGACAABgAoAAYAMAAGADgABgBAAAYASAAGAFAA"
    "BgBYAAYAYAAGAGgABgBwAAYAeAAGAIAABgCIAAYAkAAGAJgABgCgAAYAqAAGALAABgC4AAYAwAAGAMgABgDQAAYA2AAGAOAABgDo"
    "AAYA8AAGAPgABgAAAAYwAQAEAEAABgBgAAYAgAAGAKAABgDAAAYA4AAGAAABBgAgAQYAQAEGgAYAAwIAAAAAAMADHgAAeAAhAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAlgAfBAQEAAIwBZAL8BHwEf/wEfAR8BHw"
    "EfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR"
    "8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR"
    "8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHw"
    "EfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/w"
    "EfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR"
    "8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHw"
    "Ef/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHw"
    "EfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR"
    "8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR"
    "8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHw"
    "EfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR"
    "//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR"
    "8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHw"
    "EfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHw"
    "EfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR"
    "8BHwEcAOFwMDAwACA/AY8BHwEf/wEfAR8BHwEfARoJxABiCk/8AR8CHQQfAS8CjwefAR8BH/8FdQhIAK8JrwOfCbMAowBf+wGeAh"
    "0D/wVPB98CPwEfAR//BXYFmACvDt8Dnwm1Gp0O7/8CHxLvAU8BTwefAR8BHwWf9QWIAK8G7wOfBCAAogBPH3/8Ah8NrwFPB/8Hnw"
    "EfAR8Fn/8OTwb/A48EGw0fDu8DogLf/wHfAx8DfwEfAR8BHw4/Bu//A08ELxupG28CHw77A3g/j/s+J0ErAecEHQeMGM8JXw7//w"
    "pfBCw5qgaPAhkgzQrXA9/6U/4B7F/vUxIG/AyvDk8O//8OLw7/DuwXLw7lHwwZCyEP+w7/AesgeBD1AM8O/w5PDv//Dl9tLSlKHe"
    "8lmE3+Po1dj/8t6198AYYSXgysRU9Grw3//w77d08EDAIZiY5q3AEbHY//Ae8OdgLfCX8dPwFfDf8O//kLvw7tAh+Yj3pvARoprx"
    "/v/0t6Hk8O/w7/DZ8BHwzeUh//Dv0d7wx/DYsEfwL/Dv+4T/8OTw7/De8O6gZOR/8x3XqP/wr/Dv8O/wL/Dv99bzs/KO//Co8THw"
    "3no98E3w7/C397j/8B7bnqJXsSvx0/He8KHwEf+gLPDu8O/w7rDI126GyfAe/9yOp+jzaPDk8O/wp/IhgN7/+VbyLe0W9L+tWbJ4"
    "uGi2/v/+lfY28s7woPCt/vzO5d77//A40OP1cPQe947wvPp8/vv//vvw8fDt8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR"
    "8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHw"
    "EfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEZAL"
    "FwEBAQACAfAY8BHwEf/wEfAR8BHwEfARoJxABiCk/8AR8CHQQfAS8CjwefAR8BH/8FdQhIAK8JrwOfCbMAowBfSwGeAh0D/gUwAQ"
    "MQAA//Bs8BHwEfAR0PCACtAV8DLf8EMgmwAAAiAHEa9ADUAY/+Ah4IHQ2UA68GvwEfAR8BH/8KwQW4AKgArwMvBCYJsw7/8Q7mCz"
    "cE5wIfDa8FLgKPB4//AR8BHwrGBZgArwb/A58EH/EogQ7hAF8O5wIaA9kQzwU//wbvAR8BHwEfBY8O7wbvA//5AsIwAgNLDu8CHw"
    "72AUdOL/Qwez4nQSQAxQFPRO85fw5P/w7/Df8O8wr3BYwO7wIZDu/7DvhOBEAFU/0B5mtaVbpTH/UDvwrNOG8mLw58BCEFaxpf9j"
    "//AhgO6jwBCcROlzCXAh/4AeczNQYGDuMB9ALcQd8OT/8O/w4vXhYIKAE5Du0CFw7/9FvaJtsO9QD+AeVrVmvUAY/2IdAG/zLUAO"
    "gAr2DfDm0xH/8FZjEMAhmJhi+2B4tc+AyP8w5/AecQCQDmIjs5nzs/O+//Di8O8QWHAxss3AIfmI4L7/8O8ACNLlgB7iFnMToIjw"
    "5P/w7/Dg8O8xgqDfZZrgIWDg/za5sNSilpCIcqbwHtHmcO//M86AyfOz877w4vguZoXQ7v9VXWAhMIw5a1ALhI7w0FE3//Dv4OfV"
    "6SAM0O7zs/O+8Ob/9tA0efDucobA71HedV7gt/9x+WTX8B5wr5GyiiK3m/LD//LO8ObwQrHNUI9hlvBOdp3/kK9mfmTaR7hmltPv"
    "0BCYjf9TUvCs8O/w2/ARogtn/KlW//Ith2rLQu44cApwHqhohBL/ve4yV1AL8s7yzvDc8BHO2v/ADgBG/lrZ6e85aGaaHYGJ/7Al"
    "gheQV3IeYO7+8P778OH//qqF7ygEsA80MfE+QC3QRP9Uy0PicBCAJ0BjQAeQF4A//5BM8Kywt/Cg8KyQQLCkwLL/oIHgOaBDcSJw"
    "ycCMsB6w7//QV/A8cGiACvCg8ObwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR"
    "//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR"
    "8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BHg8BHwEfARBAQCAgK/AAIC8BjwEfAR8BHwEfAR//AR"
    "8BGgnEAGIKTAEfAh0EH/8BLwKPB58BHwEfBXUISACv/wmvA58JswCjAFsBngIdA///BU8H3wI/AR8BHwV2BZgAr/8O3wOfCbUanQ"
    "7vAh8S7wFP/wFPB58BHwEfBZUFiACvBu//A58EIACiAE8ffAIfDa8BT/8H/wefAR8BHwWfDk8G/wOP/wQbDR8O7wOiAt8B3wMfA3"
    "//AR8BHwEfDj8G7wNPBC8br/kbbwIfDvsDeD+LPidBKwHv9wQdB4wYzwlfDv8KXwQsOa/6Bo8CGSDNCtcD2lP+Aexf7/9TEgb8DK"
    "8OTw7/Di8O/w7v/BcvDuUfDBkLIQsO/wHrIH/4EPUAzw7/Dk8O/w5fbS0pT/od7yWYTf4+jV2PLetffAGP9hJeDKxFT0avDf8O+3"
    "dPBA/8AhmJjmrcARsdjwHvDnYC3/8Jfx0/AV8N/w75C78O7QIf/5iPem8BGimvH+9Leh5PDv//Dv8NnwEfDN5SHw79He8Mf/8Niw"
    "R/Av8O/7hPDk8O/w3v/w7qBk5H/zHdeo8K/w7/Dv//Av8O/31vOz8o7wqPEx8N7/ej3wTfDv8Lf3uPAe256iV/+xK/HT8d7wofAR"
    "oCzw7vDv//DusMjXbobJ8B7cjqfo82j/8OTw7/Cn8iGA3vlW8i3tFv/0v61Zsni4aLb+/pX2NvLO//Cg8K3+/M7l3vvwONDj9XD/"
    "9B73jvC8+nz++/778PHw7f/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHw"
    "EfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR"
    "8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR"
    "8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHw"
    "EfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/w"
    "EfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR"
    "8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHw"
    "Ef/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHw"
    "EfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR"
    "8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR"
    "8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHw"
    "EfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR"
    "//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR"
    "8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHw"
    "EfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHw"
    "EfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR"
    "8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/"
    "8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHw"
    "EfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR"
    "8BH/8BHwEfAR8BHwEfAR8BHwEf/wEfAR8BHwEfAR8BHwEfAR//AR8BHwEfAR8BHwEfAR8BH48BHwEfAR8BGgDAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAP//////////////////////////////////";


struct RomInfo {
    std::string path;
    std::string name;
    std::vector<uint8_t> data;
    uint32_t start = 0;
    uint32_t original_entry_target = 0;
    uint8_t entry_type = 9;   // capture-derived catalog type byte
    bool entry_type_overridden = false;
    uint8_t mapping_flag = 3; // low catalog mapping/config byte; captures use 3,4,5,6
    bool mapping_flag_overridden = false;
};

static void print_hex(const uint8_t* p, size_t n, size_t max = 64)
{
    const size_t shown = std::min(n, max);
    for (size_t i = 0; i < shown; ++i) {
        if (i && (i % 16) == 0) std::cout << '\n';
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(p[i]) << ' ';
    }
    if (shown < n) std::cout << "...";
    std::cout << std::dec << '\n';
}

static uint32_t read_le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

static void write_le16(std::vector<uint8_t>& v, size_t off, uint16_t x)
{
    v.at(off + 0) = static_cast<uint8_t>(x >> 0);
    v.at(off + 1) = static_cast<uint8_t>(x >> 8);
}

static void write_le32(std::vector<uint8_t>& v, size_t off, uint32_t x)
{
    v.at(off + 0) = static_cast<uint8_t>(x >> 0);
    v.at(off + 1) = static_cast<uint8_t>(x >> 8);
    v.at(off + 2) = static_cast<uint8_t>(x >> 16);
    v.at(off + 3) = static_cast<uint8_t>(x >> 24);
}

static uint32_t align_up_u32(uint32_t x, uint32_t a)
{
    return (x + a - 1u) & ~(a - 1u);
}

static std::vector<uint8_t> decode_base64(const char* s)
{
    std::array<int, 256> table{};
    table.fill(-1);
    const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < alphabet.size(); ++i)
        table[static_cast<unsigned char>(alphabet[i])] = static_cast<int>(i);

    std::vector<uint8_t> out;
    uint32_t accum = 0;
    int bits = 0;

    for (const unsigned char c : std::string(s)) {
        if (c == '=') break;
        if (std::isspace(c)) continue;
        const int val = table[c];
        if (val < 0) continue;

        accum = (accum << 6) | static_cast<uint32_t>(val);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((accum >> bits) & 0xFFu));
        }
    }
    return out;
}

static bool bulk_out(libusb_device_handle* h,
                     const uint8_t* p,
                     size_t n,
                     unsigned timeout_ms = USB_TIMEOUT_MS)
{
    return ezfadvance::BulkTransport(h).out(p, n, timeout_ms);
}

static bool bulk_out(libusb_device_handle* h,
                     const std::vector<uint8_t>& data,
                     unsigned timeout_ms = USB_TIMEOUT_MS)
{
    return bulk_out(h, data.data(), data.size(), timeout_ms);
}

// Legacy Windows EZF Advance III DLL WriteBulkNonDMAData behavior:
// after sending the 13-byte command, it busy-waits 0x2EE microseconds
// (750 us) before sending the associated data payload. The DLL constant at
// 0x10011458 is exactly 1e-6 and the delay routine uses QueryPerformanceCounter.
// Reproduce that timing rather than the earlier 16 ms / transport experiments.
static void legacy_command_data_settle()
{
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::microseconds(COMMAND_DATA_SETTLE_US);
    while (std::chrono::steady_clock::now() < deadline) {
        // Intentional busy-wait: mirrors the original DLL.
    }
}

static void precise_gap_us(uint32_t us)
{
    if (us == 0) return;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < deadline) {
        // Intentional busy-wait. For this diagnostic we want a repeatable
        // command-to-command packet gap without scheduler granularity noise.
    }
}

static bool bulk_out_paced64(libusb_device_handle* h,
                             const uint8_t* p,
                             size_t n,
                             uint32_t packet_gap_us,
                             double* elapsed_seconds = nullptr)
{
    const auto started = std::chrono::steady_clock::now();
    size_t off = 0;
    while (off < n) {
        const size_t piece = std::min<size_t>(64, n - off);
        if (!bulk_out(h, p + off, piece, USB_TIMEOUT_MS))
            return false;
        off += piece;
        if (off < n && packet_gap_us != 0)
            precise_gap_us(packet_gap_us);
    }

    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    if (elapsed_seconds) *elapsed_seconds = elapsed;
    return true;
}

static bool bulk_in_exact(libusb_device_handle* h,
                          std::vector<uint8_t>& data,
                          size_t wanted,
                          unsigned timeout_ms = USB_TIMEOUT_MS)
{
    return ezfadvance::BulkTransport(h).inExact(data, wanted, timeout_ms);
}

static bool bulk_in_max(libusb_device_handle* h,
                        std::vector<uint8_t>& data,
                        size_t max_len,
                        unsigned timeout_ms = USB_TIMEOUT_MS)
{
    return ezfadvance::BulkTransport(h).inMax(data, max_len, timeout_ms);
}

static std::vector<uint8_t> cmd92_2()
{
    return ezfadvance::Protocol::command92Two();
}

static std::vector<uint8_t> cmd92_1(uint8_t selector)
{
    return ezfadvance::Protocol::command92One(selector);
}

static bool command_data_echo(libusb_device_handle* h,
                              const std::vector<uint8_t>& command,
                              const std::vector<uint8_t>& data,
                              const std::string& label,
                              unsigned timeout_ms = USB_TIMEOUT_MS)
{
    return ezfadvance::Protocol(h).commandDataEcho(
        command, data, label, {timeout_ms, COMMAND_DATA_SETTLE_US, true});
}

static bool tx92_2(libusb_device_handle* h,
                   uint8_t a, uint8_t b,
                   const std::string& label)
{
    return command_data_echo(h, cmd92_2(), {a,b}, label);
}

static bool tx92_1(libusb_device_handle* h,
                   uint8_t selector, uint8_t value,
                   const std::string& label)
{
    return command_data_echo(h, cmd92_1(selector), {value}, label);
}

// Full bridge initialization captured in initialize.pcap and independently
// observed on the real device:
//
//   0x97 -> one byte 00
//   0x98 -> one byte 01
//   0x99 with parameter 01 -> 13-byte echo
//
// v4/v5 only sent 0x98.  That was sufficient for simple reads and erase
// traffic, but both versions programmed only the first ~0xF2 bytes of a
// 64 KiB block.  The legacy DLL exposes separate DMA/non-DMA bulk paths, and
// the 0x99 parameter is therefore a strong candidate for enabling/configuring
// the bridge's large-transfer path.  v6 restores the complete known-good
// initialization instead of relying on the bridge's previous session state.

// ---------------------------------------------------------------------------
// Original EZ3Manager card-initialization / flash-probe phase.
//
// This sequence is copied from the behavior proven by v22 on Apple Silicon.
// It is deliberately kept intact in v23.  Once ordinary writing is stable,
// individual probes can be removed experimentally to find the minimum prime.
// ---------------------------------------------------------------------------

static bool manager_tx92_2(libusb_device_handle* h,
                           uint8_t a, uint8_t b,
                           const std::string& label)
{
    return tx92_2(h, a, b, label);
}

static bool manager_tx92_1(libusb_device_handle* h,
                           uint8_t selector, uint8_t value,
                           const std::string& label)
{
    return tx92_1(h, selector, value, label);
}

static bool manager_tx92_2_at(libusb_device_handle* h,
                              uint32_t word_address,
                              uint8_t a,
                              uint8_t b,
                              const std::string& label)
{
    std::vector<uint8_t> command = cmd92_2();
    write_le32(command, 4, word_address);
    return command_data_echo(h, command, {a, b}, label);
}

static bool manager_read91_sub2_4(libusb_device_handle* h,
                                  std::array<uint8_t, 4>& response)
{
    const std::vector<uint8_t> command = {
        0x5A,0xA5,0x91,0x02,
        0,0,0,0,
        0x04,0,0,0,0
    };

    if (!bulk_out(h, command))
        return false;

    std::vector<uint8_t> in;
    if (!bulk_in_exact(h, in, 4))
        return false;

    std::copy(in.begin(), in.end(), response.begin());
    return true;
}

static bool manager_read91_normal(libusb_device_handle* h,
                                  uint32_t word_address,
                                  uint8_t* data,
                                  size_t length)
{
    std::vector<uint8_t> command(13, 0);
    command[0] = 0x5A;
    command[1] = 0xA5;
    command[2] = 0x91;
    command[3] = 0x00;
    write_le32(command, 4, word_address);
    write_le32(command, 8, static_cast<uint32_t>(length));

    if (!bulk_out(h, command))
        return false;

    std::vector<uint8_t> in;
    if (!bulk_in_exact(h, in, length))
        return false;

    std::copy(in.begin(), in.end(), data);
    return true;
}

static bool original_bridge_startup_979899(libusb_device_handle* h)
{
    const std::vector<uint8_t> c97 = {
        0x5A,0xA5,0x97,0,0,0,0,0,0,0,0,0,0
    };
    const std::vector<uint8_t> c98 = {
        0x5A,0xA5,0x98,0,0,0,0,0,0,0,0,0,0
    };
    const std::vector<uint8_t> c99 = {
        0x5A,0xA5,0x99,0,0x01,0,0,0,0,0,0,0,0
    };

    std::vector<uint8_t> response;

    if (!bulk_out(h, c97) ||
        !bulk_in_exact(h, response, 1) ||
        response[0] != 0x00) {
        std::cerr << "Original-manager 0x97 startup failed.\n";
        return false;
    }
    std::cout << "0x97 -> 00\n";

    // Cartridge-presence/readiness gate.  On the hardware-proven startup path
    // an inserted, ready EZ-Flash Advance III returns one byte 0x01 to 0x98.
    // A run with no cartridge inserted fails at this exact step.  Keep the
    // protocol unchanged, but turn that condition into an explicit preflight
    // diagnostic so the program stops before any erase/program operation.
    bool ready = false;
    for (unsigned attempt = 1; attempt <= READINESS_ATTEMPTS; ++attempt) {
        if (!bulk_out(h, c98)) {
            std::cerr
                << "\nCARTRIDGE PREFLIGHT FAILED: could not send the 0x98 "
                   "readiness command.\n"
                << "Check the ezfadvanceIII USB connection and try again.\n"
                << "No erase or program operation was attempted.\n";
            return false;
        }
        if (!bulk_in_exact(h, response, 1)) {
            std::cerr
                << "\nGBA CARTRIDGE NOT DETECTED / NOT READY.\n"
                << "The ezfadvanceIII device did not return the required 0x98 "
                   "readiness byte (expected 01).\n"
                << "No erase or program operation was attempted.\n";
            return false;
        }
        if (response[0] == 0x01) {
            ready = true;
            break;
        }
        if (response[0] != 0x00) {
            std::cerr << "\nUnexpected 0x98 readiness value 0x"
                      << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned>(response[0])
                      << std::dec << std::setfill(' ') << ".\n"
                      << "No erase or program operation was attempted.\n";
            return false;
        }
        if (attempt != READINESS_ATTEMPTS) {
            std::cout << "0x98 readiness returned 00; retrying ("
                      << attempt << '/' << READINESS_ATTEMPTS << ")...\n";
            std::this_thread::sleep_for(READINESS_RETRY_DELAY);
        }
    }
    if (!ready) {
        std::cerr
            << "\nGBA CARTRIDGE NOT DETECTED / NOT READY.\n"
            << "0x98 readiness remained 0x00 after " << READINESS_ATTEMPTS
            << " checks.\n"
            << "Make sure an EZ-Flash Advance III cartridge is fully inserted, "
               "then retry.\n"
            << "No erase or program operation was attempted.\n";
        return false;
    }
    std::cout << "0x98 -> 01 (cartridge ready)\n";

    if (!bulk_out(h, c99) ||
        !bulk_in_exact(h, response, c99.size()) ||
        response != c99) {
        std::cerr << "Original-manager 0x99 echo failed.\n";
        return false;
    }
    std::cout << "0x99 echo OK\n";
    return true;
}

static bool manager_probe_unlock_tail(libusb_device_handle* h)
{
    if (!manager_tx92_2(h, 0xAA, 0x55, "manager AA55")) return false;
    if (!manager_tx92_2(h, 0x00, 0x00, "manager zero 1")) return false;
    if (!manager_tx92_2(h, 0x00, 0x00, "manager zero 2")) return false;
    if (!manager_tx92_2(h, 0x00, 0x00, "manager zero 3")) return false;
    if (!manager_tx92_1(h, 0x00, 0xAA, "manager selector0 AA")) return false;
    if (!manager_tx92_1(h, 0x00, 0x55, "manager selector0 55")) return false;
    if (!manager_tx92_1(h, 0x01, 0x06, "manager selector1 06")) return false;
    return true;
}

static bool manager_probe_prefix(libusb_device_handle* h,
                                 uint8_t a0, uint8_t a1,
                                 uint8_t b0, uint8_t b1,
                                 uint8_t c0, uint8_t c1,
                                 bool include_tail = true)
{
    if (!manager_tx92_2(h, 0x55, 0xAA, "manager 55AA")) return false;
    if (!manager_tx92_2(h, a0, a1, "manager probe word 1")) return false;
    if (!manager_tx92_2(h, b0, b1, "manager probe word 2")) return false;
    if (!manager_tx92_2(h, c0, c1, "manager probe word 3")) return false;

    std::this_thread::sleep_for(std::chrono::milliseconds(125));

    if (include_tail)
        return manager_probe_unlock_tail(h);
    return true;
}

static bool manager_probe_reset_after_id(libusb_device_handle* h,
                                         bool use_f0)
{
    if (!manager_tx92_2(h,
                        use_f0 ? 0xF0 : 0xFF,
                        use_f0 ? 0x00 : 0xFF,
                        "manager ID reset")) return false;
    if (!manager_tx92_1(h, 0x01, 0x04, "manager status 1/04")) return false;
    if (!manager_tx92_1(h, 0x00, 0x00, "manager status 0/00 #1")) return false;
    if (!manager_tx92_1(h, 0x00, 0x00, "manager status 0/00 #2")) return false;
    return true;
}

static bool manager_flash_id_probe(libusb_device_handle* h,
                                   const char* label,
                                   uint8_t a0, uint8_t a1,
                                   uint8_t b0, uint8_t b1,
                                   uint8_t c0, uint8_t c1)
{
    if (!manager_probe_prefix(h, a0,a1,b0,b1,c0,c1))
        return false;

    if (!manager_tx92_2(h, 0x90, 0x00, "manager 90/00"))
        return false;

    std::array<uint8_t, 4> id{};
    if (!manager_read91_sub2_4(h, id))
        return false;

    std::cout << label << " ID/readback = ";
    print_hex(id.data(), id.size(), id.size());

    if (!manager_probe_reset_after_id(h, false))
        return false;

    return true;
}

static bool original_manager_initialize_and_check(libusb_device_handle* h)
{
    std::cout << "\n========================================\n"
              << "ORIGINAL MANAGER CARD INITIALIZATION / PROBE\n"
              << "========================================\n"
              << "Replaying the capture-derived state-setting phase (originally proven on macOS / Apple Silicon).\n";

    if (!original_bridge_startup_979899(h))
        return false;

    if (!manager_probe_prefix(h, 0,0, 0,0, 0,0))
        return false;

    if (!manager_tx92_2_at(h, 0x555, 0xAA, 0x00, "manager @555 AA00"))
        return false;
    if (!manager_tx92_2_at(h, 0x2AA, 0x55, 0x00, "manager @2AA 5500"))
        return false;
    if (!manager_tx92_2_at(h, 0x555, 0x90, 0x00, "manager @555 9000"))
        return false;

    std::array<uint8_t, 4> first_probe{};
    if (!manager_read91_sub2_4(h, first_probe))
        return false;
    std::cout << "initial 0x555/0x2AA probe readback = ";
    print_hex(first_probe.data(), first_probe.size(), first_probe.size());

    if (!manager_tx92_2(h, 0x90, 0x00, "manager post-initial 90/00"))
        return false;
    if (!manager_probe_reset_after_id(h, true))
        return false;

    if (!manager_flash_id_probe(
            h, "window 0x0000",
            0x00,0x00, 0x00,0x00, 0x00,0x00))
        return false;

    if (!manager_flash_id_probe(
            h, "window 0x4000",
            0x02,0x00, 0x00,0x40, 0x00,0x00))
        return false;

    if (!manager_flash_id_probe(
            h, "window 0x8000",
            0x02,0x00, 0x00,0x80, 0x00,0x00))
        return false;

    if (!manager_flash_id_probe(
            h, "window 0xC000",
            0x02,0x00, 0x00,0xC0, 0x00,0x00))
        return false;

    // The original-manager probe above has now exercised all four EZ3 ROM
    // windows.  Our capture-derived programming code proves that each window
    // corresponds to 8 MiB.  Therefore the observed address geometry is
    // 4 * 8 MiB = 32 MiB = 256 Mbit.
    //
    // Important: this is deliberately described as a geometry inference, not
    // a JEDEC density decode.  We have not yet proven how to distinguish a
    // smaller device that mirrors upper window selects, so do not use this
    // number to expand write limits automatically.
    std::cout << "\n========================================\n"
              << "CARTRIDGE INFORMATION\n"
              << "========================================\n"
              << "Status              : inserted / ready\n"
              << "EZ3 probe windows   : 4 x 8 MiB\n"
              << "Capacity (inferred) : 32 MiB / 256 Mbit\n"
              << "Detection basis     : responding EZ3 window geometry\n"
              << "Writer support      : full 32 MiB / 256 Mbit (0.5.6; capture-derived geometry)\n"
              << "Note                : capacity is not yet JEDEC-density decoded\n";

    if (!manager_flash_id_probe(
            h, "final probe window",
            0x00,0x00, 0x00,0x00, 0x02,0x00))
        return false;

    if (!manager_probe_prefix(h, 0,0, 0,0, 0,0, false))
        return false;

    const std::vector<uint8_t> c95 = {
        0x5A,0xA5,0x95,0x00,
        0x80,0x00,0x00,0x00,
        0,0,0,0,0
    };
    std::vector<uint8_t> c95_echo;

    if (!bulk_out(h, c95) ||
        !bulk_in_exact(h, c95_echo, c95.size()) ||
        c95_echo != c95) {
        std::cerr << "0x95/0x80 manager command failed.\n";
        return false;
    }
    std::cout << "0x95 parameter 0x80 echo OK\n";

    std::array<uint8_t, 0xAC> header_read{};
    if (!manager_read91_normal(
            h, 0, header_read.data(), header_read.size()))
        return false;

    std::cout << "manager-style card content check: first 0xAC bytes read OK\n"
              << "first 4 bytes = ";
    print_hex(header_read.data(), 4, 4);

    std::cout << "Original-manager initialization/probe phase complete.\n";
    return true;
}

static bool initialize_bridge(libusb_device_handle* h)
{
    std::cout << "\n========================================\n"
              << "CAPTURE-FAITHFUL STARTUP\n"
              << "========================================\n"
              << "No 0x97 and no 0x99 are sent.\n"
              << "Using the XP-proven successful startup: 3 x 0x98 polls.\n";

    const std::vector<uint8_t> c98 = {
        0x5A,0xA5,0x98,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
    };

    std::vector<uint8_t> r;

    for (unsigned i = 0; i < 3; ++i) {
        std::cout << "0x98 poll " << (i + 1) << "/3 OUT\n";
        if (!bulk_out(h, c98, 5000)) return false;
        if (!bulk_in_exact(h, r, 1, 5000)) return false;
        if (r[0] != 0x01) {
            std::cerr << "0x98 returned 0x" << std::hex
                      << static_cast<unsigned>(r[0]) << std::dec
                      << ", expected 01\n";
            return false;
        }
        std::cout << "0x98 response = 01\n";

        if (i + 1 < 3)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "post-poll quiet interval: 1000 ms\n";
    std::this_thread::sleep_for(std::chrono::seconds(1));

    return true;
}

// Capture-derived four-bank setup that precedes selective 0x96 erase operations
// in writeromonemptycard/add3rom. These are NOT the 0x96 erases themselves.
static bool captured_global_write_setup(libusb_device_handle* h,
                                        bool paced,
                                        uint32_t packet_gap_us,
                                        bool verbose = true)
{
    if (verbose) {
        std::cout << "\n========================================\n"
                  << "GLOBAL WRITE SETUP (capture-derived)\n"
                  << "========================================\n";
        if (paced)
            std::cout << "32 KiB setup transport: 64-byte calls, "
                      << packet_gap_us << " us inter-packet gap\n";
        else
            std::cout << "32 KiB setup transport: one libusb bulk transfer (baseline)\n";
    }

    const std::vector<uint8_t> cmd32k = {
        0x5A,0xA5,0x92,0x01,
        0x00,0x00,0x00,0x00,
        0x00,0x80,0x00,0x00,0x00
    };

    const std::vector<uint16_t> values = {
        0x0900, 0x0910, 0x0920, 0x0930
    };
    const std::vector<uint8_t> zeros32k(0x8000, 0x00);

    for (size_t bank = 0; bank < values.size(); ++bank) {
        const uint8_t lo = static_cast<uint8_t>(values[bank] & 0xFF);
        const uint8_t hi = static_cast<uint8_t>(values[bank] >> 8);

        if (verbose) {
            std::cout << "setup bank " << (bank + 1) << "/4 value 0x"
                      << std::hex << std::setw(4) << std::setfill('0')
                      << values[bank] << std::dec << '\n';
        }

        if (!tx92_2(h,0x55,0xAA,"GLOBAL 55AA")) return false;
        if (!tx92_2(h,0x00,0x00,"GLOBAL 0000 A")) return false;
        if (!tx92_2(h,0x00,0x00,"GLOBAL 0000 B")) return false;
        if (!tx92_2(h,lo,hi,"GLOBAL BANK VALUE")) return false;
        if (!tx92_2(h,0x00,0x00,"GLOBAL 0000 C")) return false;
        if (!tx92_2(h,0x00,0x00,"GLOBAL 0000 D")) return false;
        if (!tx92_2(h,0x00,0x00,"GLOBAL 0000 E")) return false;
        if (!tx92_2(h,0x00,0x00,"GLOBAL 0000 F")) return false;

        if (!bulk_out(h, cmd32k)) return false;
        legacy_command_data_settle();

        const auto started = std::chrono::steady_clock::now();
        double stream_elapsed = 0.0;
        if (paced) {
            if (!bulk_out_paced64(h, zeros32k.data(), zeros32k.size(),
                                  packet_gap_us, &stream_elapsed))
                return false;
        } else {
            if (!bulk_out(h, zeros32k)) return false;
            stream_elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
        }

        std::vector<uint8_t> echo;
        if (!bulk_in_max(h, echo, 64)) return false;
        if (echo != cmd32k) {
            std::cerr << "32 KiB setup command echo mismatch\n";
            return false;
        }

        if (verbose) {
            const double kib_s = stream_elapsed > 0.0
                ? 32.0 / stream_elapsed : 0.0;
            std::cout << "    setup stream: " << std::fixed
                      << std::setprecision(3) << stream_elapsed << " s, "
                      << std::setprecision(1) << kib_s << " KiB/s"
                      << std::defaultfloat << '\n';
        }
    }
    return true;
}

// Bank-0 setup/unlock sequence. This exact sequence occurs before selective
// erases and again immediately before programming in the writer captures.
static bool flash_bank0_setup(libusb_device_handle* h,
                              uint32_t pre_aa55_delay_us = 125000,
                              bool verbose_delay = false)
{
    if (!tx92_2(h,0x55,0xAA,"BANK0 55AA")) return false;
    if (!tx92_2(h,0x00,0x00,"BANK0 0000 A")) return false;
    if (!tx92_2(h,0x00,0x00,"BANK0 0000 B")) return false;
    if (!tx92_2(h,0x00,0x00,"BANK0 0000 C")) return false;

    // Every successful Windows writer capture has a repeatable ~125 ms
    // quiet interval here, immediately before AA55.  Earlier macOS writers
    // reproduced the bytes but not this state-settle interval.
    if (pre_aa55_delay_us != 0) {
        if (verbose_delay)
            std::cout << "    pre-AA55 unlock settle: "
                      << pre_aa55_delay_us << " us\n";
        std::this_thread::sleep_for(
            std::chrono::microseconds(pre_aa55_delay_us));
    }

    if (!tx92_2(h,0xAA,0x55,"BANK0 AA55")) return false;
    if (!tx92_2(h,0x00,0x00,"BANK0 0000 D")) return false;
    if (!tx92_2(h,0x00,0x00,"BANK0 0000 E")) return false;
    if (!tx92_2(h,0x00,0x00,"BANK0 0000 F")) return false;
    if (!tx92_1(h,0x00,0xAA,"BANK0 AA")) return false;
    if (!tx92_1(h,0x00,0x55,"BANK0 55")) return false;
    if (!tx92_1(h,0x01,0x06,"BANK0 06")) return false;
    return true;
}

// Second 8-MiB program/erase window from writerom128Mb.pcap.
//
// The capture switches into this window with:
//   55AA, 0200, 0040, 0000, ~125 ms, AA55, 0000 x3, AA,55,06.
//
// Program addresses then restart from local word address 0x000000.
// Captured exact-boundary verification may use a global-linear mapping.
// 0.5.2 deliberately does not invent a local-read mapping for partial BANK1:
// the 0.5.1 experiment proved that selecting the program window does not make
// subsequent local 0x91 reads address that physical window.
static bool flash_bank1_setup(libusb_device_handle* h,
                              uint32_t pre_aa55_delay_us = 125000,
                              bool verbose_delay = false)
{
    if (!tx92_2(h,0x55,0xAA,"BANK1 55AA")) return false;
    if (!tx92_2(h,0x02,0x00,"BANK1 0200")) return false;
    if (!tx92_2(h,0x00,0x40,"BANK1 0040")) return false;
    if (!tx92_2(h,0x00,0x00,"BANK1 0000 A")) return false;

    if (pre_aa55_delay_us != 0) {
        if (verbose_delay)
            std::cout << "    BANK1 pre-AA55 unlock settle: "
                      << pre_aa55_delay_us << " us\n";
        std::this_thread::sleep_for(
            std::chrono::microseconds(pre_aa55_delay_us));
    }

    if (!tx92_2(h,0xAA,0x55,"BANK1 AA55")) return false;
    if (!tx92_2(h,0x00,0x00,"BANK1 0000 B")) return false;
    if (!tx92_2(h,0x00,0x00,"BANK1 0000 C")) return false;
    if (!tx92_2(h,0x00,0x00,"BANK1 0000 D")) return false;
    if (!tx92_1(h,0x00,0xAA,"BANK1 AA")) return false;
    if (!tx92_1(h,0x00,0x55,"BANK1 55")) return false;
    if (!tx92_1(h,0x01,0x06,"BANK1 06")) return false;
    return true;
}

// Third and fourth 8-MiB windows proven by fireemblem.pcap and
// 256MBits-rom.pcap. They use the same unlock sequence as BANK1, changing
// only the 0x0080 / 0x00C0 window-select data word.
static bool flash_bank2_setup(libusb_device_handle* h,
                              uint32_t pre_aa55_delay_us = 125000,
                              bool verbose_delay = false)
{
    if (!tx92_2(h,0x55,0xAA,"BANK2 55AA")) return false;
    if (!tx92_2(h,0x02,0x00,"BANK2 0200")) return false;
    if (!tx92_2(h,0x00,0x80,"BANK2 0080")) return false;
    if (!tx92_2(h,0x00,0x00,"BANK2 0000 A")) return false;
    if (pre_aa55_delay_us != 0) {
        if (verbose_delay)
            std::cout << "    BANK2 pre-AA55 unlock settle: "
                      << pre_aa55_delay_us << " us\n";
        std::this_thread::sleep_for(std::chrono::microseconds(pre_aa55_delay_us));
    }
    if (!tx92_2(h,0xAA,0x55,"BANK2 AA55")) return false;
    if (!tx92_2(h,0x00,0x00,"BANK2 0000 B")) return false;
    if (!tx92_2(h,0x00,0x00,"BANK2 0000 C")) return false;
    if (!tx92_2(h,0x00,0x00,"BANK2 0000 D")) return false;
    if (!tx92_1(h,0x00,0xAA,"BANK2 AA")) return false;
    if (!tx92_1(h,0x00,0x55,"BANK2 55")) return false;
    if (!tx92_1(h,0x01,0x06,"BANK2 06")) return false;
    return true;
}

static bool flash_bank3_setup(libusb_device_handle* h,
                              uint32_t pre_aa55_delay_us = 125000,
                              bool verbose_delay = false)
{
    if (!tx92_2(h,0x55,0xAA,"BANK3 55AA")) return false;
    if (!tx92_2(h,0x02,0x00,"BANK3 0200")) return false;
    if (!tx92_2(h,0x00,0xC0,"BANK3 00C0")) return false;
    if (!tx92_2(h,0x00,0x00,"BANK3 0000 A")) return false;
    if (pre_aa55_delay_us != 0) {
        if (verbose_delay)
            std::cout << "    BANK3 pre-AA55 unlock settle: "
                      << pre_aa55_delay_us << " us\n";
        std::this_thread::sleep_for(std::chrono::microseconds(pre_aa55_delay_us));
    }
    if (!tx92_2(h,0xAA,0x55,"BANK3 AA55")) return false;
    if (!tx92_2(h,0x00,0x00,"BANK3 0000 B")) return false;
    if (!tx92_2(h,0x00,0x00,"BANK3 0000 C")) return false;
    if (!tx92_2(h,0x00,0x00,"BANK3 0000 D")) return false;
    if (!tx92_1(h,0x00,0xAA,"BANK3 AA")) return false;
    if (!tx92_1(h,0x00,0x55,"BANK3 55")) return false;
    if (!tx92_1(h,0x01,0x06,"BANK3 06")) return false;
    return true;
}

static bool flash_status_sequence(libusb_device_handle* h)
{
    if (!tx92_2(h,0xFF,0xFF,"STATUS FFFF")) return false;
    if (!tx92_1(h,0x01,0x04,"STATUS 04")) return false;
    if (!tx92_1(h,0x00,0x00,"STATUS 00 A")) return false;
    if (!tx92_1(h,0x00,0x00,"STATUS 00 B")) return false;
    return true;
}


// fireemblem.pcap ends programming in window 0x0080 with only a short
// loader tail beyond 16 MiB. The original manager then performs status/reset
// plus the four-write linear-read prefix before 0x91 verification. Keep this
// sequence restricted to that tiny-tail geometry; a larger partial BANK2
// multi-ROM image previously produced a boundary false negative.

static bool erase_sector(libusb_device_handle* h,
                         uint32_t word_address,
                         size_t index,
                         size_t total,
                         bool verbose)
{
    std::vector<uint8_t> cmd = {
        0x5A,0xA5,0x96,0x00,
        static_cast<uint8_t>(word_address >> 0),
        static_cast<uint8_t>(word_address >> 8),
        static_cast<uint8_t>(word_address >> 16),
        static_cast<uint8_t>(word_address >> 24),
        0x00,0x00,0x00,0x00,0x00
    };

    if (!bulk_out(h, cmd, 5000)) return false;

    std::vector<uint8_t> response;
    if (!bulk_in_max(h, response, 64, 5000)) return false;

    if (response.size() != 13 ||
        !std::equal(cmd.begin(), cmd.begin() + 12, response.begin())) {
        std::cerr << "Unexpected 0x96 response at word address 0x"
                  << std::hex << word_address << std::dec << '\n';
        return false;
    }

    if (response[12] != 0x00) {
        std::cerr << "0x96 erase returned nonzero status at word address 0x"
                  << std::hex << word_address << ": 0x"
                  << static_cast<unsigned>(response[12]) << std::dec << '\n';
        return false;
    }

    if (verbose) {
        std::cout << "  erase " << (index + 1) << "/" << total
                  << " @ word 0x" << std::hex << std::setw(6)
                  << std::setfill('0') << word_address
                  << " (byte 0x" << std::setw(6) << (word_address * 2u)
                  << ")" << std::dec << '\n';
    }
    return true;
}

static std::vector<uint32_t> selective_bank0_erase_addresses(size_t programmed_size)
{
    std::vector<uint32_t> out;

    // The capture always erases the boot-sector group first.
    for (uint32_t a = 0x0000; a <= 0x8000; a += 0x1000)
        out.push_back(a);

    // In add3rom, subsequent 64 KiB byte regions map to 0x96 word addresses
    // 0x10000, 0x18000, 0x20000, ...
    // This reproduces the exact add3rom address list for its 0x51900-byte image.
    for (size_t byte_start = 0x20000;
         byte_start < programmed_size;
         byte_start += 0x10000) {
        out.push_back(static_cast<uint32_t>(byte_start / 2));
    }

    return out;
}

static std::vector<uint32_t> full_128mb_bank0_erase_addresses()
{
    std::vector<uint32_t> out;

    // Exact first group from writerom128Mb.pcap: 135 erases.
    // Boot sectors: 0x0000..0x8000 in 0x1000-word steps.
    for (uint32_t a = 0x0000; a <= 0x8000; a += 0x1000)
        out.push_back(a);

    // Remaining bank-0 sectors: 0x10000..0x3F8000 in 0x8000-word steps.
    for (uint32_t a = 0x10000; a <= 0x3F8000; a += 0x8000)
        out.push_back(a);

    return out;
}

static std::vector<uint32_t> full_128mb_bank1_erase_addresses()
{
    std::vector<uint32_t> out;

    // Exact second group from writerom128Mb.pcap: 135 erases.
    for (uint32_t a = 0x0000; a <= 0x3F8000; a += 0x8000)
        out.push_back(a);

    // The last 64-KiB-byte region is split into seven additional 0x1000-word
    // erase sectors in the capture.
    for (uint32_t a = 0x3F9000; a <= 0x3FF000; a += 0x1000)
        out.push_back(a);

    return out;
}

static bool erase_image_capture_faithful(libusb_device_handle* h,
                                         size_t programmed_size,
                                         bool verbose)
{
    if (programmed_size <= FLASH_WINDOW_SIZE) {
        const auto addresses = selective_bank0_erase_addresses(programmed_size);
        std::cout << "\n========================================\n"
                  << "SELECTIVE SECTOR ERASE\n"
                  << "========================================\n";
        ProgressBar progress("Erase", addresses.size(), false, !verbose);
        for (size_t i = 0; i < addresses.size(); ++i) {
            if (!erase_sector(h, addresses[i], i, addresses.size(), verbose))
                return false;
            progress.update(i + 1);
        }
        return true;
    }

    const auto even_window = full_128mb_bank0_erase_addresses();
    const auto odd_window  = full_128mb_bank1_erase_addresses();

    // fireemblem.pcap erases just 0x96 address 0 in window 2 for its 0x700-byte
    // loader tail. For larger window-2 payloads, v35 conservatively uses the
    // full even-window erase geometry proven by 256MBits-rom.pcap.
    const bool tiny_window2_tail =
        programmed_size > CARD_HALF_SIZE &&
        programmed_size <= CARD_HALF_SIZE + 0x2000;

    std::vector<std::pair<unsigned, std::vector<uint32_t>>> groups;
    groups.push_back({0, even_window});
    groups.push_back({1, odd_window});
    if (programmed_size > CARD_HALF_SIZE) {
        if (tiny_window2_tail)
            groups.push_back({2, std::vector<uint32_t>{0x0000}});
        else
            groups.push_back({2, even_window});
    }
    if (programmed_size > CARD_HALF_SIZE + FLASH_WINDOW_SIZE)
        groups.push_back({3, odd_window});

    size_t total = 0;
    for (const auto& g : groups) total += g.second.size();

    std::cout << "\n========================================\n"
              << "MULTI-WINDOW ERASE (capture-derived)\n"
              << "========================================\n";
    if (verbose) {
        for (const auto& g : groups)
            std::cout << "window " << g.first << ": " << g.second.size()
                      << " erase commands\n";
    }

    ProgressBar progress("Erase", total, false, !verbose);
    size_t index = 0;
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const unsigned window = groups[gi].first;
        if (gi != 0) {
            if (!flash_status_sequence(h)) return false;
            if (verbose) {
                std::cout << "switching erase window to " << window
                          << " (card byte 0x" << std::hex
                          << (static_cast<size_t>(window) * FLASH_WINDOW_SIZE)
                          << std::dec << ")...\n";
            }
            bool ok = false;
            if (window == 1) ok = flash_bank1_setup(h, 125000, verbose);
            else if (window == 2) ok = flash_bank2_setup(h, 125000, verbose);
            else if (window == 3) ok = flash_bank3_setup(h, 125000, verbose);
            if (!ok) return false;
        }
        for (uint32_t a : groups[gi].second) {
            if (!erase_sector(h, a, index, total, verbose)) return false;
            ++index;
            progress.update(index);
        }
    }
    return true;
}

static std::vector<uint8_t> make_program_command(uint32_t byte_address,
                                                 uint32_t length,
                                                 uint8_t final_byte)
{
    if (byte_address & 1u)
        throw std::runtime_error("program address must be word-aligned");

    const uint32_t word_address = byte_address / 2u;
    std::vector<uint8_t> c = {
        0x5A,0xA5,0x92,0x00,
        0,0,0,0,
        0,0,0,0,
        final_byte
    };
    write_le32(c, 4, word_address);
    write_le32(c, 8, length);
    return c;
}

static bool program_transaction_single(libusb_device_handle* h,
                                       uint32_t byte_address,
                                       const uint8_t* data,
                                       size_t n,
                                       bool verbose)
{
    const auto cmd = make_program_command(
        byte_address, static_cast<uint32_t>(n), 0x41);

    if (!bulk_out(h, cmd)) return false;
    legacy_command_data_settle();

    const auto started = std::chrono::steady_clock::now();
    if (!bulk_out(h, data, n, USB_TIMEOUT_MS)) return false;
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();

    std::vector<uint8_t> completion;
    if (!bulk_in_max(h, completion, 64)) return false;

    auto expected = cmd;
    expected[12] = 0x00;
    if (completion != expected) {
        std::cerr << "Program completion response mismatch at byte 0x"
                  << std::hex << byte_address << std::dec << '\n';
        std::cerr << "Expected:\n";
        print_hex(expected.data(), expected.size(), expected.size());
        std::cerr << "Received:\n";
        if (!completion.empty())
            print_hex(completion.data(), completion.size(), completion.size());
        return false;
    }

    if (verbose) {
        const double kib_s = elapsed > 0.0
            ? (static_cast<double>(n) / 1024.0) / elapsed : 0.0;
        std::cout << "    single data request: " << std::fixed
                  << std::setprecision(3) << elapsed << " s, "
                  << std::setprecision(1) << kib_s << " KiB/s"
                  << std::defaultfloat << '\n';
    }
    return true;
}

static bool program_image(libusb_device_handle* h,
                          const std::vector<uint8_t>& image,
                          bool verbose)
{
    std::cout << "\n========================================\n"
              << "PROGRAMMING IMAGE (MANAGER-PRIMED STATE)\n"
              << "========================================\n"
              << "One 0x92 transaction + one BULK OUT per Windows-sized block\n";

    ProgressBar progress("Program", image.size(), true, !verbose);

    for (size_t off = 0; off < image.size(); ) {
        if (off != 0 && (off % FLASH_WINDOW_SIZE) == 0) {
            const unsigned window = static_cast<unsigned>(off / FLASH_WINDOW_SIZE);
            if (window > 3) {
                std::cerr << "Program window index exceeds captured 256-Mbit geometry.\n";
                return false;
            }
            if (!flash_status_sequence(h)) return false;
            if (verbose) {
                std::cout << "\nSwitching program window to " << window
                          << " (card byte 0x" << std::hex << off << std::dec
                          << ")...\n";
            }
            bool ok = false;
            if (window == 1) ok = flash_bank1_setup(h, 125000, verbose);
            else if (window == 2) ok = flash_bank2_setup(h, 125000, verbose);
            else if (window == 3) ok = flash_bank3_setup(h, 125000, verbose);
            if (!ok) return false;
        }

        const size_t n = std::min(PROGRAM_BLOCK, image.size() - off);
        const size_t local_off = off % FLASH_WINDOW_SIZE;
        if (verbose) {
            std::cout << "program card byte 0x" << std::hex << std::setw(8)
                      << std::setfill('0') << off
                      << " local byte 0x" << std::setw(6) << local_off
                      << " length 0x" << n << std::dec << '\n';
        }

        if (!program_transaction_single(
                h, static_cast<uint32_t>(local_off),
                image.data() + off, n, verbose))
            return false;
        off += n;
        progress.update(off);
    }
    return true;
}

static std::optional<uint32_t> arm_branch_target(const std::vector<uint8_t>& rom)
{
    if (rom.size() < 4) return std::nullopt;
    return ezfadvance::CartridgeFormat::armBranchTarget(
        read_le32(rom.data()));
}

static bool contains_bytes(const std::vector<uint8_t>& data,
                           const std::string& needle)
{
    if (needle.empty() || data.size() < needle.size()) return false;
    return std::search(data.begin(), data.end(),
                       needle.begin(), needle.end()) != data.end();
}

// Catalog entry type is a ROM-size class, not a save-type code. Independent
// original-manager captures now line up exactly with:
//   32 MiB -> 0, 16 MiB -> 1, 8 MiB -> 2, 4 MiB -> 3, ... 64 KiB -> 9.
//
// In particular, 4MiB-4MiB.pcap uses type 3 for both F-Zero (SRAM_V111) and
// Mario Kart (FLASH_V124), while 4_4_8MiB.pcap uses type 2 for 8-MiB Advance
// Wars 2 (FLASH_V126) and type 3 for both 4-MiB games. The same rule explains
// all older captured 8/16/32-MiB and small-homebrew entries without per-title
// or per-save-signature exceptions.
//
// Official dumps are normally power-of-two sized. For an unusual non-power-of-
// two file, use the next power-of-two class, matching the existing placement
// allocator. --typeN remains available for protocol experiments.
struct NonSramSaveInfo {
    std::string family;
    std::string signature;
};

static std::optional<std::string> detect_ascii_library_signature(
    const std::vector<uint8_t>& rom,
    const char* prefix,
    size_t prefix_len)
{
    const auto it = std::search(rom.begin(), rom.end(), prefix, prefix + prefix_len);
    if (it == rom.end())
        return std::nullopt;

    const size_t off = static_cast<size_t>(it - rom.begin());
    std::string sig;
    for (size_t i = off; i < rom.size() && sig.size() < 20; ++i) {
        const uint8_t c = rom[i];
        if (c < 0x20 || c > 0x7E)
            break;
        sig.push_back(static_cast<char>(c));
    }
    if (sig.empty())
        sig.assign(prefix, prefix_len);
    return sig;
}

static std::optional<NonSramSaveInfo> detect_non_sram_save_library(
    const std::vector<uint8_t>& rom)
{
    // Check the more-specific FLASH prefixes first because FLASH1M_V and
    // FLASH512_V are separate SDK library families from the generic FLASH_V.
    struct Candidate {
        const char* prefix;
        size_t len;
        const char* family;
    };
    static constexpr Candidate candidates[] = {
        {"FLASH1M_V",  9, "FLASH1M (128 KiB Flash)"},
        {"FLASH512_V", 10, "FLASH512 (64 KiB Flash)"},
        {"FLASH_V",     7, "FLASH (64 KiB Flash)"},
        {"EEPROM_V",    8, "EEPROM"},
    };

    for (const auto& c : candidates) {
        if (const auto sig = detect_ascii_library_signature(rom, c.prefix, c.len))
            return NonSramSaveInfo{c.family, *sig};
    }
    return std::nullopt;
}

static bool warn_and_confirm_non_sram_save(const RomInfo& r,
                                           const NonSramSaveInfo& info)
{
    std::cerr
        << "\n========================================\n"
        << "WARNING: NON-SRAM SAVE FORMAT\n"
        << "========================================\n"
        << "ROM: " << r.path << '\n'
        << "Detected save library: " << info.signature << '\n'
        << "Embedded save-library signature family: " << info.family << "\n\n"
        << "This marker normally identifies a non-SRAM GBA save library. However,\n"
        << "manual SRAM patching can leave the original FLASH/EEPROM signature in\n"
        << "the ROM, so the marker does NOT prove the active runtime save method.\n\n"
        << "This writer NEVER patches or converts ROM save routines automatically.\n"
        << "If SRAM conversion is needed, do it manually with a separate save-patching\n"
        << "tool BEFORE running this writer. If the ROM is already SRAM-patched, this\n"
        << "signature warning may be safely expected.\n"
        << "========================================\n"
        << "Continue anyway? [y/N]: " << std::flush;

    std::string answer;
    if (!std::getline(std::cin, answer)) {
        std::cerr << "\nNo response received; aborting safely.\n";
        return false;
    }

    // Trim surrounding whitespace and accept only an explicit y/yes.
    const auto first = answer.find_first_not_of(" \t\r\n");
    const auto last  = answer.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) {
        std::cerr << "No selected; aborting safely.\n";
        return false;
    }

    std::string normalized = answer.substr(first, last - first + 1);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (normalized == "y" || normalized == "yes") {
        std::cerr << "Yes selected; continuing.\n";
        return true;
    }

    std::cerr << "No selected; aborting safely.\n";
    return false;
}

static uint8_t detect_entry_type(const std::vector<uint8_t>& rom)
{
    if (rom.size() > MAX_CARD_IMAGE)
        throw std::runtime_error("ROM file exceeds 256-Mbit cartridge capacity");

    // Type 9 is the 64-KiB class. Each doubling of the ROM size decreases the
    // type by one, reaching type 0 at 32 MiB. Clamp tiny/homebrew files to the
    // capture/program granularity and round unusual sizes up to the next class.
    uint64_t size_class = PROGRAM_BLOCK;
    uint8_t type = 9;
    while (size_class < rom.size()) {
        size_class <<= 1;
        if (type == 0 || size_class > MAX_CARD_IMAGE)
            throw std::runtime_error("ROM size class exceeds cartridge geometry");
        --type;
    }
    return type;
}

static bool has_flash_save_library(const std::vector<uint8_t>& rom)
{
    // Captured original-manager cases:
    //   FLASH_V121     -> map 6 (Advance Wars)
    //   FLASH_V124     -> map 6 (Mario Kart)
    //   FLASH_V126     -> map 6 (Advance Wars 2)
    //   FLASH512_V130  -> map 6 (FFTA)
    // FLASH1M_Vxxx is the same Nintendo/SDK FLASH save-library family and is
    // handled generically here rather than adding version-specific exceptions.
    return contains_bytes(rom, "FLASH_V") ||
           contains_bytes(rom, "FLASH512_V") ||
           contains_bytes(rom, "FLASH1M_V");
}

// The low byte of catalog entry bytes 20..23 is independent from ROM size.
// Captured SRAM/non-FLASH cases use 3; captured FLASH save-library cases use 6.
// EEPROM is now known to have at least two distinct values:
//   * Classic NES / EEPROM_V124 captures -> map 4
//   * Tales of Phantasia / EEPROM_V124   -> map 5
// Therefore EEPROM_Vnnn alone is not a generic discriminator.
static bool has_eeprom_save_library(const std::vector<uint8_t>& rom)
{
    return contains_bytes(rom, "EEPROM_V");
}

static uint8_t detect_mapping_flag(const std::vector<uint8_t>& rom)
{
    // EEPROM is intentionally excluded: callers must require an explicit
    // --mapN override until a generic capacity/configuration discriminator is
    // recovered from captures or ROM structure.
    if (has_flash_save_library(rom)) return 6;
    return 3;
}

static size_t meaningful_rom_end(const std::vector<uint8_t>& rom)
{
    // writeonerom8MB.pcap proves that Windows can place the single-ROM loader
    // *inside* a power-of-two ROM file's trailing FF padding rather than after
    // the physical file size.  For that capture the loader begins at 0x2CC420
    // even though the programmed image is a full 8 MiB.
    //
    // Use the final non-FF byte as the meaningful data extent.  Keep at least
    // the GBA header/entry area even for pathological files.
    size_t end = rom.size();
    while (end > 0xC0 && rom[end - 1] == 0xFF)
        --end;
    return end;
}


static uint32_t next_power_of_two_u32(uint32_t x)
{
    if (x <= 1u) return 1u;
    --x;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    return x + 1u;
}

static uint32_t multi_rom_size_class_alignment(size_t file_size)
{
    // The two new original-manager captures are consistent with a buddy-like
    // size-class allocator: an 8-MiB ROM is placed on an 8-MiB boundary, while
    // the 64-KiB Piano image is placed on a 64-KiB boundary. Use the next
    // power-of-two size class for non-power-of-two input files, with 64 KiB as
    // the minimum capture/program granularity.
    if (file_size > MAX_CARD_IMAGE)
        throw std::runtime_error("ROM file exceeds 256-Mbit cartridge capacity");

    uint32_t wanted = static_cast<uint32_t>(
        std::max<size_t>(PROGRAM_BLOCK, file_size));
    const uint32_t a = next_power_of_two_u32(wanted);
    if (a == 0 || a > MAX_CARD_IMAGE)
        throw std::runtime_error("ROM size class exceeds cartridge geometry");
    return a;
}

static uint64_t align_up_u64(uint64_t x, uint64_t a)
{
    return (x + a - 1u) & ~(a - 1u);
}

static std::optional<uint32_t> find_ff_slot_in_image(
    const std::vector<uint8_t>& image,
    size_t required_size)
{
    size_t i = 0xC0;
    const size_t limit = std::min(image.size(), MAX_CARD_IMAGE);

    while (i < limit) {
        while (i < limit && image[i] != 0xFF)
            ++i;
        const size_t run_start = i;
        while (i < limit && image[i] == 0xFF)
            ++i;
        const size_t run_end = i;
        if (run_end <= run_start)
            continue;

        const size_t aligned = (run_start + 15u) & ~static_cast<size_t>(15u);
        if (aligned <= run_end && required_size <= run_end - aligned)
            return static_cast<uint32_t>(aligned);
    }
    return std::nullopt;
}

static std::optional<uint32_t> find_internal_ff_loader_slot(
    const std::vector<uint8_t>& rom,
    size_t required_size)
{
    // writerom128Mb.pcap contains FFTA_INTER. (AFXP) and proves how the
    // original manager handles a ROM whose physical file already fills the
    // 16-MiB card: it places the single-ROM loader into an internal erased
    // 0xFF run instead of extending the image beyond 0x1000000.
    //
    // In that capture the selected slot is 0x00D97730.  The bytes immediately
    // before it show the erased run begins at 0x00D97728, so the manager's
    // chosen address is the first 16-byte-aligned location in that gap.
    //
    // Preserve all smaller-ROM behavior: this search is only used by the
    // single-ROM builder when appending the loader would exceed card capacity.
    size_t i = 0xC0;
    const size_t limit = std::min(rom.size(), MAX_CARD_IMAGE);

    while (i < limit) {
        while (i < limit && rom[i] != 0xFF)
            ++i;

        const size_t run_start = i;

        while (i < limit && rom[i] == 0xFF)
            ++i;

        const size_t run_end = i;
        if (run_end <= run_start)
            continue;

        // Single-ROM captures show the manager reserves a gap large enough for
        // the full multi-ROM loader footprint even though the single loader is
        // only 0x660 bytes. Tales has an earlier 0x65C4-byte FF run that is
        // large enough for 0x660 but smaller than 0x7080; EZ3Manager skips it
        // and chooses the next large run at 0xD49010. It then starts at the
        // *next* 16-byte boundary (0xD49020), not the run-start address itself.
        const size_t aligned =
            (run_start + 16u) & ~static_cast<size_t>(15u);
        const size_t reserve_size = std::max(required_size, MULTI_TEMPLATE_LEN);

        if (aligned <= run_end &&
            reserve_size <= run_end - aligned) {
            return static_cast<uint32_t>(aligned);
        }
    }

    return std::nullopt;
}

static std::string derive_name(const std::string& path)
{
    // Avoid std::filesystem so the same source builds cleanly on older BSD
    // C++17 toolchains as well as Linux and macOS. Accept both Unix '/' and
    // Windows-style '\\' separators because ROM paths may come from shared
    // folders mounted inside a VM.
    const size_t slash = path.find_last_of("/\\");
    std::string stem =
        (slash == std::string::npos) ? path : path.substr(slash + 1);

    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos && dot != 0)
        stem.resize(dot);

    std::string out;
    for (unsigned char c : stem) {
        if (out.size() >= 16) break;
        if (c >= 0x20 && c <= 0x7E)
            out.push_back(static_cast<char>(c));
        else
            out.push_back('_');
    }

    if (out.empty()) out = "ROM";
    return out;
}

static bool load_rom(RomInfo& r)
{
    std::ifstream f(r.path, std::ios::binary);
    if (!f) {
        std::cerr << "Cannot open ROM: " << r.path << '\n';
        return false;
    }

    r.data.assign(
        std::istreambuf_iterator<char>(f),
        std::istreambuf_iterator<char>());

    if (r.data.empty()) {
        std::cerr << "ROM is empty: " << r.path << '\n';
        return false;
    }

    const auto target = arm_branch_target(r.data);
    if (!target) {
        std::cerr << "ROM does not begin with the capture-supported "
                     "EAxxxxxx ARM branch: " << r.path << '\n';
        return false;
    }

    if (const auto non_sram = detect_non_sram_save_library(r.data)) {
        if (!warn_and_confirm_non_sram_save(r, *non_sram)) {
            std::cerr << "ROM processing cancelled before image construction; "
                         "no USB write was attempted.\n";
            return false;
        }
    }

    r.original_entry_target = *target;
    if (r.name.empty()) r.name = derive_name(r.path);
    if (!r.entry_type_overridden)
        r.entry_type = detect_entry_type(r.data);

    if (!r.mapping_flag_overridden) {
        if (has_eeprom_save_library(r.data)) {
            std::cerr
                << "EEPROM catalog mapping is ambiguous for: " << r.path << '\n'
                << "Current captures prove EEPROM_V124 can use map 4 or map 5.\n"
                << "Specify the capture-appropriate value explicitly with "
                   "--mapN=4 or --mapN=5 for this ROM slot.\n"
                << "No USB write was attempted.\n";
            return false;
        }
        r.mapping_flag = detect_mapping_flag(r.data);
    }

    return true;
}

static size_t relocate_template(std::vector<uint8_t>& t,
                                uint32_t original_base,
                                uint32_t new_base)
{
    const uint32_t lo = GBA_ROM_BASE + original_base;
    const uint32_t hi = lo + static_cast<uint32_t>(t.size());
    const int64_t delta = static_cast<int64_t>(new_base) -
                          static_cast<int64_t>(original_base);

    size_t count = 0;

    // The two independent single-ROM captures prove that all required
    // relocation words are 4-byte aligned literals. Including `hi` is
    // important: the template contains pointers to its one-past-end address.
    for (size_t off = 0; off + 4 <= t.size(); off += 4) {
        const uint32_t v = read_le32(t.data() + off);
        if (v >= lo && v <= hi) {
            const uint32_t nv =
                static_cast<uint32_t>(static_cast<int64_t>(v) + delta);
            write_le32(t, off, nv);
            ++count;
        }
    }
    return count;
}

static void write_catalog_entry(std::vector<uint8_t>& t,
                                size_t off,
                                const RomInfo& r,
                                bool first)
{
    if (off + 28 > t.size())
        throw std::runtime_error("catalog entry is outside template");

    std::fill(t.begin() + static_cast<std::ptrdiff_t>(off),
              t.begin() + static_cast<std::ptrdiff_t>(off + 28),
              0x00);

    for (size_t i = 0; i < r.name.size() && i < 16; ++i)
        t[off + i] = static_cast<uint8_t>(r.name[i]);

    // Capture entry bytes 16..19:
    // GBA-Notepad => 00 00 00 07
    // PIANO/Bios  => 00 00 00 09
    const uint32_t type_field = static_cast<uint32_t>(r.entry_type) << 24;
    write_le32(t, off + 16, type_field);

    // Capture entry bytes 20..23:
    //   high 24 bits: ((byte_start / 2) << 8)
    //   low byte:     ROM mapping/config flag
    //
    // Captured SRAM cases use flag 3.  Both captured FLASH cases use flag 6:
    //   FFTA / FLASH512_V130 (16 MiB) -> 6
    //   Advance Wars / FLASH_V121 (8 MiB) -> 6
    // Therefore flag 6 is not a 128-Mbit-size special case.
    const uint32_t packed_start =
        ((r.start / 2u) << 8) | static_cast<uint32_t>(r.mapping_flag);

    write_le32(t, off + 20, packed_start);

    // The first ROM stores its original ARM branch target (0xC0 in all
    // supplied ROMs). Later ROMs store their byte start address.
    write_le32(t, off + 24,
               first ? r.original_entry_target : r.start);
}

static std::vector<uint8_t> build_single_image(std::vector<RomInfo>& roms)
{
    if (roms.size() != 1)
        throw std::runtime_error("single image builder requires exactly one ROM");

    auto t = decode_base64(kSingleTemplateB64);
    if (t.size() != SINGLE_TEMPLATE_LEN)
        throw std::runtime_error("single template decode length mismatch");

    RomInfo& r = roms[0];
    r.start = 0;

    const size_t meaningful_end = meaningful_rom_end(r.data);
    uint32_t loader_start = align_up_u32(static_cast<uint32_t>(meaningful_end), 16u);

    // Original-manager single-ROM captures reserve a multi-loader-sized FF
    // footprint even though only the 0x660-byte single loader is copied. Tales
    // has only 0x800 bytes of trailing FF, so EZ3Manager correctly ignores that
    // tail and uses the large internal gap at 0xD49010 instead.
    const size_t single_slot_reserve = std::max(t.size(), MULTI_TEMPLATE_LEN);

    bool loader_fits_in_padding = false;
    if (loader_start <= r.data.size() &&
        static_cast<uint64_t>(loader_start) + single_slot_reserve <= r.data.size()) {
        loader_fits_in_padding = std::all_of(
            r.data.begin() + static_cast<std::ptrdiff_t>(loader_start),
            r.data.begin() + static_cast<std::ptrdiff_t>(loader_start + single_slot_reserve),
            [](uint8_t b) { return b == 0xFF; });
    }

    bool loader_fits_in_internal_gap = false;
    bool loader_in_second_half = false;

    if (!loader_fits_in_padding) {
        if (r.data.size() == CARD_HALF_SIZE) {
            // Preserve FFTA first: the original manager uses its internal FF
            // gap even on a 256-Mbit cartridge.
            if (const auto internal = find_internal_ff_loader_slot(r.data, t.size())) {
                loader_start = *internal;
                loader_fits_in_internal_gap = true;
            } else {
                // fireemblem.pcap exact fallback for a full 16-MiB ROM with no
                // suitable internal FF hole.
                loader_start = static_cast<uint32_t>(CARD_HALF_SIZE + 0x10);
                loader_in_second_half = true;
            }
        } else {
            loader_start = align_up_u32(static_cast<uint32_t>(r.data.size()), 16u);
            if (static_cast<uint64_t>(loader_start) + t.size() > MAX_CARD_IMAGE) {
                const auto internal = find_internal_ff_loader_slot(r.data, t.size());
                if (!internal) {
                    throw std::runtime_error(
                        "single ROM fills the 32-MiB card and no internal FF "
                        "gap is large enough for the single-ROM loader");
                }
                loader_start = *internal;
                loader_fits_in_internal_gap = true;
            }
        }
    }

    const size_t reloc_count = relocate_template(t, SINGLE_TEMPLATE_BASE, loader_start);
    if (reloc_count != 13)
        throw std::runtime_error("unexpected single-template relocation count");

    write_le16(t, SINGLE_HEADER_OFF, 1);
    write_le16(t, SINGLE_HEADER_COUNT2_OFF, 1);

    write_catalog_entry(t, SINGLE_ENTRY_OFF, r, true);

    if (r.mapping_flag == 6) {
        std::cout << "FLASH catalog mapping: type "
                  << static_cast<unsigned>(r.entry_type)
                  << ", mapping flag 6 (capture-derived)\n";
    }

    size_t image_size = std::max<size_t>(
        r.data.size(), static_cast<size_t>(loader_start) + t.size());
    if (loader_in_second_half)
        image_size = (image_size + 0xFFu) & ~static_cast<size_t>(0xFFu);

    if (image_size > MAX_CARD_IMAGE)
        throw std::runtime_error("single-ROM image exceeds 256-Mbit cartridge capacity");

    std::vector<uint8_t> image(image_size, 0xFF);
    std::copy(r.data.begin(), r.data.end(), image.begin());
    if (loader_in_second_half) {
        std::fill(image.begin() + static_cast<std::ptrdiff_t>(CARD_HALF_SIZE),
                  image.begin() + static_cast<std::ptrdiff_t>(loader_start),
                  0x00);
    }

    const uint32_t patched =
        ezfadvance::CartridgeFormat::makeArmBranch(loader_start);
    write_le32(image, 0, patched);
    std::copy(t.begin(), t.end(),
              image.begin() + static_cast<std::ptrdiff_t>(loader_start));

    std::cout << "Single-ROM meaningful data end @ byte 0x"
              << std::hex << meaningful_end << std::dec << '\n';
    std::cout << "Single-ROM loader @ byte 0x"
              << std::hex << loader_start << std::dec;
    if (loader_fits_in_padding)
        std::cout << " (inside trailing FF padding)";
    else if (loader_fits_in_internal_gap)
        std::cout << " (inside internal FF gap)";
    else if (loader_in_second_half)
        std::cout << " (second 16-MiB half; fireemblem.pcap behavior)";
    std::cout << '\n';

    return image;
}

static std::vector<uint8_t> build_multi_image(std::vector<RomInfo>& roms)
{
    if (roms.size() < 2)
        throw std::runtime_error("multi image builder requires at least two ROMs");
    if (roms.size() > MULTI_CATALOG_MAX_ENTRIES) {
        std::ostringstream oss;
        oss << "multi-ROM catalog has " << MULTI_CATALOG_MAX_ENTRIES
            << " structural slots, but " << roms.size() << " ROMs were supplied";
        throw std::runtime_error(oss.str());
    }

    const std::vector<std::string> input_paths = [&]() {
        std::vector<std::string> v;
        v.reserve(roms.size());
        for (const auto& r : roms) v.push_back(r.path);
        return v;
    }();

    // piano-megamanz.pcap and 8_8_16MiB.pcap both prove that EZ3Manager does
    // not preserve insertion order. The observed rule is a stable descending
    // file-size order: larger ROMs move first, while equal-size ROMs retain
    // their original relative order.
    std::stable_sort(
        roms.begin(), roms.end(),
        [](const RomInfo& a, const RomInfo& b) {
            return a.data.size() > b.data.size();
        });

    bool reordered = false;
    for (size_t i = 0; i < roms.size(); ++i) {
        if (roms[i].path != input_paths[i]) {
            reordered = true;
            break;
        }
    }

    std::cout << "\n========================================\n"
              << "CAPTURE-DERIVED MULTI-ROM PACKING\n"
              << "========================================\n"
              << "Ordering: stable descending ROM file size\n";
    if (reordered)
        std::cout << "Input order changed to match original EZ3Manager packing.\n";
    else
        std::cout << "Input order already matches original EZ3Manager packing.\n";

    // Place each ROM according to the size-class behavior exposed by the two
    // packing captures. The allocator advances by meaningful (non-trailing-FF)
    // data, then aligns the next ROM to that next ROM's size class. This lets a
    // smaller ROM reuse trailing FF padding without allowing a large ROM to
    // straddle a boundary that EZ3Manager itself avoids.
    uint64_t meaningful_cursor = 0;
    for (size_t i = 0; i < roms.size(); ++i) {
        const size_t meaningful = meaningful_rom_end(roms[i].data);
        const uint32_t alignment = multi_rom_size_class_alignment(roms[i].data.size());

        uint64_t start = 0;
        if (i != 0)
            start = align_up_u64(meaningful_cursor, alignment);

        if (start > UINT32_MAX || start + roms[i].data.size() > MAX_CARD_IMAGE) {
            std::ostringstream oss;
            oss << "multi-ROM placement exceeds 256-Mbit cartridge while placing "
                << roms[i].name;
            throw std::runtime_error(oss.str());
        }

        roms[i].start = static_cast<uint32_t>(start);
        meaningful_cursor = std::max<uint64_t>(
            meaningful_cursor, start + meaningful);

        std::cout << "  #" << (i + 1) << " " << roms[i].name
                  << ": file 0x" << std::hex << roms[i].data.size()
                  << ", meaningful end +0x" << meaningful
                  << ", size-class align 0x" << alignment
                  << ", start 0x" << roms[i].start
                  << std::dec << '\n';
    }

    size_t base_image_size = 0;
    for (const auto& r : roms) {
        base_image_size = std::max<size_t>(
            base_image_size,
            static_cast<size_t>(r.start) + r.data.size());
    }
    if (base_image_size > MAX_CARD_IMAGE)
        throw std::runtime_error("packed ROM image exceeds 256-Mbit cartridge capacity");

    std::vector<uint8_t> image(base_image_size, 0xFF);

    // Copy in sorted/physical order. Any overlap is legal only where earlier
    // ROM bytes are erased FF padding; meaningful_cursor placement above is
    // designed to guarantee that property, but validate it explicitly.
    std::vector<uint8_t> occupied_non_ff(base_image_size, 0);
    for (const auto& r : roms) {
        const size_t base = r.start;
        for (size_t j = 0; j < r.data.size(); ++j) {
            const size_t dst = base + j;
            if (occupied_non_ff[dst] && r.data[j] != 0xFF) {
                std::ostringstream oss;
                oss << "multi-ROM packing would overlap meaningful ROM data at card byte 0x"
                    << std::hex << dst;
                throw std::runtime_error(oss.str());
            }
            if (r.data[j] != 0xFF)
                occupied_non_ff[dst] = 1;
            image[dst] = r.data[j];
        }
    }

    auto t = decode_base64(kMultiTemplateB64);
    if (t.size() != MULTI_TEMPLATE_LEN)
        throw std::runtime_error("multi template decode length mismatch");

    static_assert(MULTI_CAPTURE_GAP_OFF + MULTI_CAPTURE_GAP_BYTES.size()
                  <= MULTI_TEMPLATE_LEN,
                  "multi capture-gap patch must fit inside loader template");
    std::copy(MULTI_CAPTURE_GAP_BYTES.begin(),
              MULTI_CAPTURE_GAP_BYTES.end(),
              t.begin() + static_cast<std::ptrdiff_t>(MULTI_CAPTURE_GAP_OFF));

    size_t loader_copy_len = t.size();
    if (roms.size() == 2)
        loader_copy_len = MULTI_TWO_ROM_LEN;

    // Both new captures prove the loader is embedded in an existing erased
    // run when one is available: 0x2CC420 inside MegaManZ for MegaManZ+Piano,
    // and 0x15C2360 inside Advance Wars for FireEmblem+AdvanceWars+MegaManZ.
    uint32_t loader_start = 0;
    if (const auto slot = find_ff_slot_in_image(image, loader_copy_len)) {
        loader_start = *slot;
        std::cout << "Loader placement: internal/trailing FF slot @ 0x"
                  << std::hex << loader_start << std::dec << '\n';
    } else {
        const uint64_t appended = align_up_u64(image.size(), 16u);
        if (appended + loader_copy_len > MAX_CARD_IMAGE)
            throw std::runtime_error(
                "no capture-compatible FF slot for multi-ROM loader and no card space to append it");
        loader_start = static_cast<uint32_t>(appended);
        image.resize(static_cast<size_t>(loader_start) + loader_copy_len, 0xFF);
        occupied_non_ff.resize(image.size(), 0);
        std::cout << "Loader placement: appended @ 0x"
                  << std::hex << loader_start << std::dec
                  << " (no sufficiently large FF slot found)\n";
    }

    const size_t reloc_count =
        relocate_template(t, MULTI_TEMPLATE_BASE, loader_start);
    if (reloc_count != 125) {
        std::ostringstream oss;
        oss << "unexpected multi-template relocation count: "
            << reloc_count << " (expected 125)";
        throw std::runtime_error(oss.str());
    }

    write_le16(t, MULTI_HEADER_OFF,
               static_cast<uint16_t>(roms.size()));
    write_le16(t, MULTI_HEADER_OFF + 14,
               static_cast<uint16_t>(roms.size()));

    std::fill(
        t.begin() + static_cast<std::ptrdiff_t>(MULTI_ENTRY_OFF),
        t.begin() + static_cast<std::ptrdiff_t>(
            MULTI_ENTRY_OFF + MULTI_TEMPLATE_POPULATED_ENTRY_SLOTS * 28),
        0x00);

    for (size_t i = 0; i < roms.size(); ++i)
        write_catalog_entry(t, MULTI_ENTRY_OFF + i * 28, roms[i], i == 0);

    if (roms.size() == 2) {
        // Exact unused third-entry sentinel observed in piano-bios.pcap.
        const size_t unused = MULTI_ENTRY_OFF + 2 * 28;
        std::fill(t.begin() + static_cast<std::ptrdiff_t>(unused),
                  t.begin() + static_cast<std::ptrdiff_t>(unused + 28),
                  0x00);
        std::fill(t.begin() + static_cast<std::ptrdiff_t>(unused + 1),
                  t.begin() + static_cast<std::ptrdiff_t>(unused + 16),
                  0x20);

        std::fill(
            t.begin() + static_cast<std::ptrdiff_t>(
                MULTI_TWO_ROM_ERASED_TAIL_OFF),
            t.begin() + static_cast<std::ptrdiff_t>(MULTI_TWO_ROM_LEN),
            0xFF);
    }

    if (roms.size() >= 4) {
        // Capture-exact >=4-ROM tail from the independent four-, five-,
        // and six-ROM captures.
        static_assert(MULTI_FOUR_PLUS_ROM_ZERO_TAIL_OFF + MULTI_FOUR_PLUS_ROM_ZERO_TAIL_LEN
                      == MULTI_TEMPLATE_LEN,
                      ">=4-ROM zero tail must end at the loader boundary");
        std::fill(
            t.begin() + static_cast<std::ptrdiff_t>(MULTI_FOUR_PLUS_ROM_ZERO_TAIL_OFF),
            t.begin() + static_cast<std::ptrdiff_t>(MULTI_TEMPLATE_LEN),
            0x00);
    }

    if (static_cast<size_t>(loader_start) + loader_copy_len > image.size())
        image.resize(static_cast<size_t>(loader_start) + loader_copy_len, 0xFF);

    // Validate that the chosen loader slot did not overlap meaningful ROM data.
    for (size_t i = 0; i < loader_copy_len; ++i) {
        const size_t dst = static_cast<size_t>(loader_start) + i;
        if (dst < occupied_non_ff.size() && occupied_non_ff[dst]) {
            std::ostringstream oss;
            oss << "loader placement overlaps meaningful ROM data at card byte 0x"
                << std::hex << dst;
            throw std::runtime_error(oss.str());
        }
    }

    // Only physical/catalog ROM #1 is patched. Reordering therefore matters:
    // the branch is applied after stable sorting, exactly as the captures show.
    const uint32_t patched =
        ezfadvance::CartridgeFormat::makeArmBranch(loader_start);
    write_le32(image, 0, patched);

    std::copy(t.begin(),
              t.begin() + static_cast<std::ptrdiff_t>(loader_copy_len),
              image.begin() + static_cast<std::ptrdiff_t>(loader_start));

    std::cout << "Multi-ROM loader/menu @ byte 0x"
              << std::hex << loader_start << std::dec << '\n';

    if (roms.size() == 2) {
        std::cout << "Two-ROM capture-exact loader extent: 0x"
                  << std::hex << loader_copy_len
                  << " bytes; image end 0x" << image.size()
                  << std::dec << '\n';
    } else if (roms.size() >= 4) {
        std::cout << roms.size()
                  << "-ROM capture-exact loader extent: 0x" << std::hex
                  << loader_copy_len << "; image end 0x" << image.size()
                  << std::dec << '\n';
    } else {
        std::cout << "Three-ROM loader extent: 0x" << std::hex
                  << loader_copy_len << "; image end 0x" << image.size()
                  << std::dec << '\n';
    }

    return image;
}

static void print_layout(const std::vector<RomInfo>& roms,
                         const std::vector<uint8_t>& image,
                         size_t programmed_size)
{
    std::cout << "\n========================================\n"
              << "IMAGE LAYOUT\n"
              << "========================================\n";

    for (size_t i = 0; i < roms.size(); ++i) {
        const auto& r = roms[i];
        std::cout << "ROM " << (i + 1)
                  << ": " << r.name
                  << "\n  file: " << r.path
                  << "\n  size: " << r.data.size()
                  << " (0x" << std::hex << r.data.size() << std::dec << ")"
                  << "\n  byte start: 0x" << std::hex << r.start << std::dec
                  << "\n  original entry target: 0x"
                  << std::hex << r.original_entry_target << std::dec
                  << "\n  catalog type: " << static_cast<unsigned>(r.entry_type)
                  << (r.entry_type_overridden ? " (override)" : " (size-class rule)")
                  << "\n  mapping flag: " << static_cast<unsigned>(r.mapping_flag)
                  << (r.mapping_flag_overridden ? " (override)" : " (signature/map rule)")
                  << "\n";
    }

    std::cout << "Constructed image bytes: " << image.size()
              << " (0x" << std::hex << image.size() << std::dec << ")\n"
              << "Programmed bytes: " << programmed_size
              << " (0x" << std::hex << programmed_size << std::dec << ")\n"
              << "Patched ROM #1 first 4 bytes: ";
    print_hex(image.data(), std::min<size_t>(4, image.size()), 4);
}

struct BuiltCartridgeImage {
    std::vector<uint8_t> bytes;
    size_t programmed_size = 0;
};

class CartridgeImageBuilder final {
public:
    bool build(std::vector<RomInfo>& roms,
               BuiltCartridgeImage& result,
               std::string& error) const
    {
        result.bytes = roms.size() == 1
            ? build_single_image(roms)
            : build_multi_image(roms);

        if (result.bytes.size() > MAX_CARD_IMAGE) {
            std::ostringstream message;
            message << "Constructed image is 0x" << std::hex
                    << result.bytes.size()
                    << " bytes, exceeding the 256-Mbit cartridge capacity 0x"
                    << MAX_CARD_IMAGE << std::dec << ".";
            error = message.str();
            return false;
        }

        const uint64_t minimum_programmed = std::max<uint64_t>(
            PROGRAM_BLOCK, static_cast<uint64_t>(result.bytes.size()));
        const uint64_t programmed = align_up_u64(minimum_programmed, 0x100u);
        if (programmed > MAX_CARD_IMAGE) {
            std::ostringstream message;
            message << "0x100-aligned programmed extent 0x" << std::hex
                    << programmed
                    << " exceeds the 256-Mbit cartridge capacity 0x"
                    << MAX_CARD_IMAGE << std::dec << ".";
            error = message.str();
            return false;
        }

        result.programmed_size = static_cast<size_t>(programmed);
        result.bytes.resize(result.programmed_size, 0xFF);
        return true;
    }
};

class CardWriter final {
public:
    CardWriter(libusb_device_handle* handle, bool verbose)
        : handle_(handle), verbose_(verbose), transport_(handle),
          verification_(transport_)
    {
    }

    bool preflight() { return original_manager_initialize_and_check(handle_); }
    bool initializeBridge() { return initialize_bridge(handle_); }
    bool prepareGlobalWrite() {
        return captured_global_write_setup(handle_, false, 0, verbose_);
    }
    bool selectWindowZeroForErase() {
        return flash_bank0_setup(handle_, 125000, false);
    }
    bool erase(std::size_t image_size) {
        return erase_image_capture_faithful(handle_, image_size, verbose_);
    }
    bool finalizeFlashState() { return flash_status_sequence(handle_); }
    bool selectWindowZeroForProgram() {
        return flash_bank0_setup(handle_, 125000, verbose_);
    }
    bool program(const std::vector<uint8_t>& image) {
        return program_image(handle_, image, verbose_);
    }
    bool verifyPartialFirstWindow(const std::vector<uint8_t>& image) {
        std::cout << "\nPreparing capture-proven partial first-window linear "
                     "read/verify state...\n"
                  << "\n========================================\n"
                  << "READ-BACK VERIFICATION (CAPTURE-LINEAR)\n"
                  << "========================================\n";

        const size_t verify_size =
            ezfadvance::VerificationSession::partialFirstWindowExtent(
                image.size());
        if (verify_size != image.size()) {
            std::cout
                << "Partial BANK0 verify extent rounded to 64-KiB block: 0x"
                << std::hex << verify_size << std::dec << " bytes\n";
        }
        ProgressBar progress("Verify", verify_size, true, !verbose_);
        return verification_.verifyPartialFirstWindow(
            image,
            [&](size_t offset, size_t length) {
                if (verbose_) {
                    std::cout << "verified card byte 0x" << std::hex
                              << std::setw(8) << std::setfill('0') << offset
                              << " length 0x" << length << std::dec << '\n';
                } else {
                    progress.update(offset + length);
                }
            });
    }
    bool verifyExact8MiB(const std::vector<uint8_t>& image) {
        std::cout
            << "\nPreparing lower-8-MiB / 64-Mbit linear read/verify mapping...\n"
            << "\n========================================\n"
            << "READ-BACK VERIFICATION (CAPTURE-LINEAR)\n"
            << "========================================\n";
        ProgressBar progress("Verify", image.size(), true, !verbose_);
        return verification_.verifyExact8MiB(
            image,
            [&](size_t offset, size_t length) {
                if (verbose_) {
                    std::cout << "verified card byte 0x" << std::hex
                              << std::setw(8) << std::setfill('0') << offset
                              << " length 0x" << length << std::dec << '\n';
                } else {
                    progress.update(offset + length);
                }
            });
    }
    bool verifyExact16MiB(const std::vector<uint8_t>& image) {
        std::cout
            << "\nPreparing 128-Mbit / 16-MiB linear read/verify mapping...\n"
            << "\n========================================\n"
            << "READ-BACK VERIFICATION (CAPTURE-LINEAR)\n"
            << "========================================\n";
        ProgressBar progress("Verify", image.size(), true, !verbose_);
        return verification_.verifyExact16MiB(
            image,
            [&](size_t offset, size_t length) {
                if (verbose_) {
                    std::cout << "verified card byte 0x" << std::hex
                              << std::setw(8) << std::setfill('0') << offset
                              << " length 0x" << length << std::dec << '\n';
                } else {
                    progress.update(offset + length);
                }
            });
    }
    bool verifyTinyTailAbove16MiB(const std::vector<uint8_t>& image) {
        const size_t verify_size =
            ezfadvance::VerificationSession::tinyTailAbove16MiBExtent(
                image.size());
        std::cout
            << "\nPreparing tiny BANK2-tail linear read/verify mapping "
               "(fireemblem.pcap)...\n"
            << "Short BANK2 tail verify extent: 0x" << std::hex
            << verify_size << std::dec << " bytes\n"
            << "\n========================================\n"
            << "READ-BACK VERIFICATION (CAPTURE-LINEAR)\n"
            << "========================================\n";
        ProgressBar progress("Verify", verify_size, true, !verbose_);
        return verification_.verifyTinyTailAbove16MiB(
            image,
            [&](size_t offset, size_t length) {
                if (verbose_) {
                    std::cout << "verified card byte 0x" << std::hex
                              << std::setw(8) << std::setfill('0') << offset
                              << " length 0x" << length << std::dec << '\n';
                } else {
                    progress.update(offset + length);
                }
            });
    }
    bool verifyExact24MiB(const std::vector<uint8_t>& image) {
        std::cout
            << "\nPreparing 192-Mbit / 24-MiB linear read/verify mapping...\n"
            << "    capture quiet interval: approximately 109 ms\n"
            << "    preserved pre-AA55 settle: 125000 us\n"
            << "\n========================================\n"
            << "READ-BACK VERIFICATION (CAPTURE-LINEAR)\n"
            << "========================================\n";
        ProgressBar progress("Verify", image.size(), true, !verbose_);
        return verification_.verifyExact24MiB(
            image,
            [&](size_t offset, size_t length) {
                if (verbose_) {
                    std::cout << "verified card byte 0x" << std::hex
                              << std::setw(8) << std::setfill('0') << offset
                              << " length 0x" << length << std::dec << '\n';
                } else {
                    progress.update(offset + length);
                }
            });
    }
    bool verifyExact32MiB(const std::vector<uint8_t>& image) {
        std::cout
            << "\nPreparing 256-Mbit / 32-MiB linear read/verify mapping...\n"
            << "    preserved pre-AA55 settle: 125000 us\n"
            << "\n========================================\n"
            << "READ-BACK VERIFICATION (CAPTURE-LINEAR)\n"
            << "========================================\n";
        ProgressBar progress("Verify", image.size(), true, !verbose_);
        return verification_.verifyExact32MiB(
            image,
            [&](size_t offset, size_t length) {
                if (verbose_) {
                    std::cout << "verified card byte 0x" << std::hex
                              << std::setw(8) << std::setfill('0') << offset
                              << " length 0x" << length << std::dec << '\n';
                } else {
                    progress.update(offset + length);
                }
            });
    }

private:
    libusb_device_handle* handle_;
    bool verbose_;
    ezfadvance::BulkTransport transport_;
    ezfadvance::VerificationSession verification_;
};

static void usage(const char* argv0)
{
    std::cerr
        << "ezfadvanceIII manager-primed ROM writer (" << host_platform_name() << ")\n\n"
        << "Dry run / inspect layout only:\n"
        << "  " << argv0 << " rom1.gba [rom2.gba ...]\n\n"
        << "Build in memory + erase/program/verify:\n"
        << "  " << argv0 << " --yes-really-write "
           "rom1.gba [rom2.gba ...]\n\n"
        << "Build in memory + erase/program without read-back verify:\n"
        << "  " << argv0 << " --yes-really-write --skip-verify "
           "rom1.gba [rom2.gba ...]\n\n"
        << "Output options:\n"
        << "  --verbose   Show per-sector/per-block erase, program, timing, and verify diagnostics.\n"
        << "              Without it, erase/program/verify use live progress bars.\n\n"
        << "Optional capture-metadata overrides:\n"
        << "  --type1=2   --type6=3   --type10=4\n"
        << "  --map1=6    --map6=6    --map10=3\n\n"
        << "Notes:\n"
        << "  * No small fixed ROM-count limit is imposed; 1..8 active entries are capture-proven.\n"
        << "  * The loader contains 120 structural catalog slots; this is a safety bound, not a claim that 120-ROM menu operation is proven.\n"
        << "  * Total input ROM file size must not exceed 32 MiB / 256 Mbit.\n"
        << "  * No intermediate .bin image is written to disk.\n"
        << "  * FLASH/FLASH512/FLASH1M/EEPROM signatures emit a conservative warning.\n"
        << "    Each warning requires an explicit y/yes response to continue.\n"
        << "    This writer never patches save routines automatically.\n"
        << "    If SRAM conversion is desired, patch the ROM manually with a separate tool first.\n"
        << "  * EEPROM_V alone does not determine map 4 vs 5; use --mapN explicitly for EEPROM ROMs.\n"
        << "  * The constructed in-memory image must fit in the 32 MiB / 256-Mbit cartridge.\n"
        << "  * Full-size single ROMs may place the loader inside trailing/internal FF space.\n"
        << "  * Catalog type is derived from ROM size class (32 MiB=0 ... 64 KiB=9).\n"
        << "  * FLASH-family metadata maps to 6; SRAM/other to 3; EEPROM requires explicit map 4/5.\n"
        << "  * Multi-ROM input is stable-sorted by descending file size, matching captures.\n"
        << "  * Equal-size ROMs keep their original relative order.\n"
        << "  * Smaller ROMs may reuse trailing FF padding of earlier ROMs.\n"
        << "  * The multi-ROM loader is embedded in a suitable FF run when possible.\n"
        << "  * Physical/catalog ROM #1 is patched to branch to the EZF loader/menu.\n"
        << "  * Every constructed image below 8 MiB is fully read-back verified.\n"
        << "  * Exact 8/16/24/32-MiB readback paths are capture-proven.\n"
        << "  * Other partial higher-window geometries remain verify-skipped until captured.\n"
        << "  * --skip-verify skips all post-write ROM read-back comparison.\n"
        << "    A short status/reset cleanup is still sent after programming.\n"
        << "  * Without --yes-really-write, no USB device is touched and nothing is written.\n";
}

int main(int argc, char** argv)
{
    try {
        ezfadvance::WriterOptions options;
        const auto parse_result = ezfadvance::WriterOptions::parse(
            argc, argv, options, std::cerr);
        if (!parse_result.ok) {
            if (parse_result.show_usage) usage(argv[0]);
            return 1;
        }
        const bool do_write = options.write;
        const bool skip_verify = options.skip_verify;
        const bool verbose = options.verbose;
        const auto& type_override = options.type_overrides;
        const auto& mapping_override = options.mapping_overrides;
        const auto& rom_paths = options.rom_paths;

        if (rom_paths.empty()) {
            usage(argv[0]);
            return 1;
        }
        if (rom_paths.size() > MULTI_CATALOG_MAX_ENTRIES) {
            std::cerr << "Too many ROMs for the embedded catalog structure: "
                      << rom_paths.size() << " supplied, "
                      << MULTI_CATALOG_MAX_ENTRIES << " slots available.\n";
            return 1;
        }

        std::vector<RomInfo> roms;
        for (size_t i = 0; i < rom_paths.size(); ++i) {
            RomInfo r;
            r.path = rom_paths[i];
            r.name = derive_name(r.path);
            if (type_override[i]) {
                r.entry_type = *type_override[i];
                r.entry_type_overridden = true;
            }
            if (mapping_override[i]) {
                r.mapping_flag = *mapping_override[i];
                r.mapping_flag_overridden = true;
            }

            if (!load_rom(r))
                return 1;

            roms.push_back(std::move(r));
        }

        uint64_t total_rom_file_bytes = 0;
        for (const auto& r : roms)
            total_rom_file_bytes += static_cast<uint64_t>(r.data.size());

        std::cout << "Total input ROM file bytes: " << total_rom_file_bytes
                  << " (0x" << std::hex << total_rom_file_bytes << std::dec
                  << "), cartridge capacity " << MAX_CARD_IMAGE
                  << " bytes / 256 Mbit\n";

        if (total_rom_file_bytes > MAX_CARD_IMAGE) {
            std::cerr << "Total input ROM file size exceeds the 256-Mbit / "
                         "32-MiB cartridge capacity.\n";
            return 1;
        }

        const CartridgeImageBuilder image_builder;
        BuiltCartridgeImage built_image;
        std::string image_build_error;
        if (!image_builder.build(roms, built_image, image_build_error)) {
            std::cerr << image_build_error << '\n';
            return 1;
        }
        std::vector<uint8_t>& image = built_image.bytes;
        const size_t programmed_size = built_image.programmed_size;

        print_layout(roms, image, programmed_size);

        if (image.size() >= CARD_HALF_SIZE) {
            std::cout
                << "\n========================================\n"
                << "LARGE-IMAGE RELIABILITY NOTE\n"
                << "========================================\n"
                << "This image is 128 Mbit / 16 MiB or larger.\n"
                << "On real EZ-Flash Advance III hardware, a preventive full-card wipe\n"
                << "with ezfadvanceIII_wipe_card before a large write has been observed to\n"
                << "improve the chance of reliable programming.\n"
                << "The wipe is optional, destructive, and is NOT run automatically.\n";
        }

        if (!do_write) {
            std::cout
                << "\nDRY RUN: no USB device was touched and the cartridge was not modified.\n"
                << "Review the layout above, then rerun with --yes-really-write "
                   "to program it.\n";
            return 0;
        }

        std::cout
            << "\nWARNING: --yes-really-write supplied.\n"
            << "This will erase sectors at the beginning of the cartridge "
               "and program the constructed image.\n";

        ezfadvance::UsbDevice device;
        const auto open_result = device.open(std::cerr);
        if (!open_result) {
            if (open_result.status == ezfadvance::UsbOpenStatus::initialization_failed) {
                std::cerr << "libusb_init failed: "
                          << libusb_error_name(open_result.libusb_error) << '\n';
            } else if (open_result.status == ezfadvance::UsbOpenStatus::device_not_found) {
                std::cerr << "ezfadvanceIII VID=0x0E6A PID=0x5088 not found.\n";
            } else {
                std::cerr << "Could not claim interface 0: "
                          << libusb_error_name(open_result.libusb_error) << '\n';
            }
            return 1;
        }
        libusb_device_handle* h = device.handle();
        CardWriter card_writer(h, verbose);

        std::cout << "ezfadvanceIII opened; interface 0 claimed.\n";

        bool ok = card_writer.preflight();

        if (!ok) {
            std::cerr
                << "\n========================================\n"
                << "WRITE PREFLIGHT ABORTED\n"
                << "========================================\n"
                << "Initialization/card check did not complete.\n"
                << "No erase or program operation was attempted.\n";

            return 2;
        }

        std::cout << "\nSimulating close-manager -> launch-v19 transition...\n";
        ok = card_writer.initializeBridge();

        std::cout << "\n========================================\n"
                  << "MANAGER-PRIMED FULL WRITE\n"
                  << "========================================\n"
                  << "Manager-prime/write transition originally proven on macOS / Apple Silicon.\n"
                  << "Flash-window pre-AA55 settle: fixed 125 ms.\n"
                  << "ROM payload: one Windows-style 64-KiB BULK OUT request.\n";

        // Reproduce the captured writer setup once, on a fresh bridge/cart
        // session, before doing any erase/program operation.
        if (ok)
            ok = card_writer.prepareGlobalWrite();

        // Capture-faithful BANK0 unlock for erase.
        if (ok)
            ok = card_writer.selectWindowZeroForErase();

        if (ok)
            ok = card_writer.erase(image.size());

        // For <=8 MiB this closes bank0 erasing.  For >8 MiB the helper has
        // already switched to bank1 and erased it, so this closes bank1.
        if (ok)
            ok = card_writer.finalizeFlashState();

        // This is the key v17 experiment: on a completely fresh run, use the
        // repeatable Windows-capture 125-ms quiet interval immediately before
        // AA55, then send the real image in the original single-request form.
        if (ok)
            ok = card_writer.selectWindowZeroForProgram();

        if (ok)
            ok = card_writer.program(image);

        bool full_verify_completed = false;
        bool full_verify_skipped = false;
        bool verify_skipped_by_user = false;

        if (ok && skip_verify) {
            // --skip-verify means no post-write 0x91 ROM read-back comparison.
            // Keep only the short non-readback status/reset cleanup so the
            // flash/bridge is not intentionally left in program-command state.
            std::cout
                << "\n--skip-verify supplied: skipping post-write ROM read-back "
                   "verification.\n"
                << "Sending non-readback flash status/reset cleanup only.\n";
            ok = card_writer.finalizeFlashState();
            verify_skipped_by_user = ok;
        } else if (ok) {
            // Use only verification mappings directly supported by captures.
            // Compact packing now turns the Piano+MegaManZ case into the
            // original manager's exact 8-MiB geometry, so it remains entirely
            // on the simple capture-linear path.
            const ezfadvance::VerificationPolicy verification_policy;
            switch (verification_policy.modeFor(image.size())) {
            case ezfadvance::VerificationMode::exact_32_mib:
                ok = card_writer.verifyExact32MiB(image);
                full_verify_completed = ok;
                break;
            case ezfadvance::VerificationMode::exact_24_mib:
                ok = card_writer.verifyExact24MiB(image);
                full_verify_completed = ok;
                break;
            case ezfadvance::VerificationMode::exact_16_mib:
                ok = card_writer.verifyExact16MiB(image);
                full_verify_completed = ok;
                break;
            case ezfadvance::VerificationMode::exact_8_mib:
                // Exact 8 MiB is independently capture-proven by both the
                // single-ROM Advance Wars capture and 4MiB-4MiB.pcap.
                ok = card_writer.verifyExact8MiB(image);
                full_verify_completed = ok;
                break;
            case ezfadvance::VerificationMode::partial_first_window:
                // 2MB.pcap, 2_2MB.pcap, and 4MB.pcap prove the generic partial
                // first-window path: status cleanup only, then ordinary linear
                // 0x91 reads. VerificationSession rounds the final read to a
                // full 64-KiB block and expects erased 0xFF beyond image.size().
                ok = card_writer.verifyPartialFirstWindow(image);
                full_verify_completed = ok;
                break;
            case ezfadvance::VerificationMode::tiny_tail_above_16_mib:
                // fireemblem.pcap proves this tiny BANK2-tail transition and
                // verifies one complete 64-KiB block beyond 16 MiB.
                ok = card_writer.verifyTinyTailAbove16MiB(image);
                full_verify_completed = ok;
                break;
            case ezfadvance::VerificationMode::unsupported_partial_higher_window:
                // 0.5.1 proved that reusing program-window selection for local
                // 0x91 verification is wrong. Do not turn an unproven mapping
                // into a false WRITE/VERIFY failure. Return the flash to a
                // normal status/read state and report the limitation clearly.
                ok = card_writer.finalizeFlashState();
                if (ok) {
                    full_verify_skipped = true;
                    std::cout
                        << "\nFull read-back verification skipped: image extent 0x"
                        << std::hex << image.size() << std::dec
                        << " is a partial higher-window geometry with no "
                           "capture-proven linear read mapping yet.\n"
                        << "Capture-proven full verification currently covers "
                           "all images below 8 MiB, exact 8/16/24/32 MiB, and "
                           "the dedicated tiny-tail-above-16-MiB case.\n"
                        << "Programming completed; no experimental verification "
                           "window selection was sent.\n";
                }
                break;
            }
        }

        std::cout << "\n========================================\n";
        if (!ok) {
            std::cout << "WRITE/FINALIZE FAILED.\n";
        } else if (verify_skipped_by_user) {
            std::cout << "WRITE SUCCEEDED; READ-BACK VERIFICATION SKIPPED BY REQUEST.\n";
        } else if (full_verify_skipped) {
            std::cout << "WRITE SUCCEEDED; FULL READ-BACK VERIFICATION SKIPPED.\n";
        } else if (full_verify_completed) {
            std::cout << "WRITE + FULL READ-BACK VERIFICATION SUCCEEDED.\n";
        } else {
            std::cout << "WRITE SUCCEEDED.\n";
        }
        std::cout << "========================================\n";

        return ok ? 0 : 2;
    }
    catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return 1;
    }
}
