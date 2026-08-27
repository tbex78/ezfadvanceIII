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
#include "ezfadvance/cartridge_image_builder.hpp"
#include "ezfadvance/card_writer.hpp"
#include "ezfadvance/libusb_writer_backend.hpp"
#include "ezfadvance/protocol.hpp"
#include "ezfadvance/verification_session.hpp"
#include "ezfadvance/writer_options.hpp"
#include "ezfadvance/verification_policy.hpp"
#include "ezfadvance/version.hpp"

static constexpr size_t PROGRAM_BLOCK = 0x10000; // 64 KiB
static constexpr size_t CARD_HALF_SIZE = 0x1000000;   // 128 Mbit / 16 MiB boundary
static constexpr size_t MAX_CARD_IMAGE = 0x2000000;   // 256 Mbit / 32 MiB card

using RomInfo = ezfadvance::RomInfo;
using BuiltCartridgeImage = ezfadvance::BuiltCartridgeImage;
using CartridgeImageBuilder = ezfadvance::CartridgeImageBuilder;
static uint32_t read_le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

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


// ezfadvanceIII multi-ROM writer 0.9.0 for macOS, Linux and BSD.
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

static void usage(const char* argv0)
{
    std::cerr
        << "ezfadvanceIII manager-primed ROM writer (" << host_platform_name() << ")\n\n"
        << "Version:\n"
        << "  " << argv0 << " --version\n\n"
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
        << "  --map1=6    --map6=6    --map10=3\n";
}

int main(int argc, char** argv)
{
    if (ezfadvance::isVersionRequest(argc, argv)) {
        ezfadvance::printVersion(std::cout, "ezfadvanceIII_multirom_writer");
        return 0;
    }

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
        if (rom_paths.size() > CartridgeImageBuilder::catalog_slots) {
            std::cerr << "Too many ROMs for the embedded catalog structure: "
                      << rom_paths.size() << " supplied, "
                      << CartridgeImageBuilder::catalog_slots
                      << " slots available.\n";
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
        std::cout << built_image.report;
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
        auto writer_backend = ezfadvance::makeLibusbWriterBackend(h, verbose);
        const ezfadvance::CardWriter card_writer(*writer_backend);

        std::cout << "ezfadvanceIII opened; interface 0 claimed.\n";

        const auto write_result = card_writer.write(image, skip_verify, std::cout);

        if (write_result.status == ezfadvance::CardWriteStatus::preflight_failed) {
            std::cerr
                << "\n========================================\n"
                << "WRITE PREFLIGHT ABORTED\n"
                << "========================================\n"
                << "Initialization/card check did not complete.\n"
                << "No erase or program operation was attempted.\n";

            return 2;
        }

        std::cout << "\n========================================\n";
        if (!write_result) {
            std::cout << "WRITE/FINALIZE FAILED.\n";
        } else if (write_result.verification_skipped_by_user) {
            std::cout << "WRITE SUCCEEDED; READ-BACK VERIFICATION SKIPPED BY REQUEST.\n";
        } else if (write_result.verification_skipped_unproven) {
            std::cout << "WRITE SUCCEEDED; FULL READ-BACK VERIFICATION SKIPPED.\n";
        } else if (write_result.verification_completed) {
            std::cout << "WRITE + FULL READ-BACK VERIFICATION SUCCEEDED.\n";
        } else {
            std::cout << "WRITE SUCCEEDED.\n";
        }
        std::cout << "========================================\n";

        return write_result ? 0 : 2;
    }
    catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return 1;
    }
}
