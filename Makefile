# EZF Advance III toolset Makefile
#
# Written using portable make/shell constructs for native make on Unix-like
# systems, including Linux, macOS, FreeBSD, OpenBSD, and NetBSD.
#
# Default:
#   make
# builds unversioned sources:
#   ezfadvanceIII_multirom_writer.cpp
#   ezfadvanceIII_card_reader.cpp
#   ezfadvanceIII_save_reader.cpp
#   ezfadvanceIII_wipe_card.cpp
#
# Optional versioned sources:
#   make VERSION=0.9.0
#
# Command-line make variables override these defaults, for example:
#   make CXX=clang++
#   make CXXFLAGS="-std=c++17 -O0 -g"
#   make WARNFLAGS="-Wall -Wextra -Wpedantic"

VERSION =

CXX = c++
CXXFLAGS = -std=c++17 -O2

# Optional stricter warnings for development:
# WARNFLAGS = -Wall -Wextra -Wpedantic
WARNFLAGS =

CPPFLAGS =
PROJECT_CPPFLAGS = -Iinclude
LDFLAGS =
LDLIBS =

WRITER = ezfadvanceIII_multirom_writer
CARD_READER = ezfadvanceIII_card_reader
SAVE_READER = ezfadvanceIII_save_reader
WIPE = ezfadvanceIII_wipe_card

PROGRAMS = $(WRITER) $(CARD_READER) $(SAVE_READER) $(WIPE)
TRANSPORT_SOURCES = src/usb_device.cpp src/protocol.cpp src/platform.cpp
WRITER_SOURCES = $(TRANSPORT_SOURCES) src/card_writer.cpp src/libusb_writer_backend.cpp src/cartridge_format.cpp src/cartridge_image_builder.cpp src/eeprom_mapping.cpp src/verification_policy.cpp src/verification_session.cpp src/writer_options.cpp
READ_ONLY_SOURCES = $(TRANSPORT_SOURCES) src/cartridge_format.cpp src/ez3_catalog.cpp src/read_only_cartridge.cpp src/read_session_transition.cpp
CARD_READER_SOURCES = $(READ_ONLY_SOURCES) src/card_reader_options.cpp
SAVE_READER_SOURCES = $(READ_ONLY_SOURCES) src/save_memory_reader.cpp src/save_memory_writer.cpp
WIPE_SOURCES = $(TRANSPORT_SOURCES)
ALL_SUPPORT_SOURCES = $(SAVE_READER_SOURCES) src/card_reader_options.cpp src/card_writer.cpp src/libusb_writer_backend.cpp src/cartridge_image_builder.cpp src/eeprom_mapping.cpp src/verification_policy.cpp src/verification_session.cpp src/writer_options.cpp

# Source selection is performed by the POSIX shell in each recipe so this
# Makefile does not depend on GNU/BSD make conditionals.
WRITER_BASE = ezfadvanceIII_multirom_writer
CARD_READER_BASE = ezfadvanceIII_card_reader
SAVE_READER_BASE = ezfadvanceIII_save_reader
WIPE_BASE = ezfadvanceIII_wipe_card

# Prefer pkg-config. Otherwise try common installation prefixes.
# Warning flags are quiet by default, so ordinary -I is sufficient and more
# compiler-portable than relying on -isystem.
LIBUSB_CFLAGS = `if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libusb-1.0 >/dev/null 2>&1; then pkg-config --cflags libusb-1.0; elif [ -f /opt/homebrew/opt/libusb/include/libusb-1.0/libusb.h ]; then printf '%s\n' '-I/opt/homebrew/opt/libusb/include'; elif [ -f /usr/local/opt/libusb/include/libusb-1.0/libusb.h ]; then printf '%s\n' '-I/usr/local/opt/libusb/include'; elif [ -f /usr/local/include/libusb-1.0/libusb.h ]; then printf '%s\n' '-I/usr/local/include'; elif [ -f /usr/include/libusb-1.0/libusb.h ]; then printf '%s\n' '-I/usr/include'; else printf '%s\n' ''; fi`

LIBUSB_LIBS = `if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libusb-1.0 >/dev/null 2>&1; then pkg-config --libs libusb-1.0; elif [ -d /opt/homebrew/opt/libusb/lib ]; then printf '%s\n' '-L/opt/homebrew/opt/libusb/lib -lusb-1.0'; elif [ -d /usr/local/opt/libusb/lib ]; then printf '%s\n' '-L/usr/local/opt/libusb/lib -lusb-1.0'; elif [ -d /usr/local/lib ]; then printf '%s\n' '-L/usr/local/lib -lusb-1.0'; else printf '%s\n' '-lusb-1.0'; fi`

ALL_CXXFLAGS = $(CXXFLAGS) $(WARNFLAGS)
ALL_CPPFLAGS = $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(LIBUSB_CFLAGS)
ALL_LDLIBS = $(LIBUSB_LIBS) $(LDLIBS)

all: $(PROGRAMS)

writer: $(WRITER)

card-reader: $(CARD_READER)

save-reader: $(SAVE_READER)

wipe: $(WIPE)

# FORCE makes the selected source get checked every time even though the
# versioned/unversioned source filename is chosen dynamically in the shell.
# This avoids non-portable make conditionals/functions.
$(WRITER): FORCE
	@base="$(WRITER_BASE)"; \
	if [ -n "$(VERSION)" ]; then src="$${base}_$(VERSION).cpp"; else src="$${base}.cpp"; fi; \
	if [ ! -f "$$src" ]; then echo "Missing source: $$src" >&2; exit 1; fi; \
	echo "$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) $$src $(WRITER_SOURCES) $(LDFLAGS) $(ALL_LDLIBS) -o $@"; \
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) "$$src" $(WRITER_SOURCES) $(LDFLAGS) $(ALL_LDLIBS) -o "$@"

$(CARD_READER): FORCE
	@base="$(CARD_READER_BASE)"; \
	if [ -n "$(VERSION)" ]; then src="$${base}_$(VERSION).cpp"; else src="$${base}.cpp"; fi; \
	if [ ! -f "$$src" ]; then echo "Missing source: $$src" >&2; exit 1; fi; \
	echo "$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) $$src $(CARD_READER_SOURCES) $(LDFLAGS) $(ALL_LDLIBS) -o $@"; \
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) "$$src" $(CARD_READER_SOURCES) $(LDFLAGS) $(ALL_LDLIBS) -o "$@"

$(SAVE_READER): FORCE
	@base="$(SAVE_READER_BASE)"; \
	if [ -n "$(VERSION)" ]; then src="$${base}_$(VERSION).cpp"; else src="$${base}.cpp"; fi; \
	if [ ! -f "$$src" ]; then echo "Missing source: $$src" >&2; exit 1; fi; \
	echo "$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) $$src $(SAVE_READER_SOURCES) $(LDFLAGS) $(ALL_LDLIBS) -o $@"; \
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) "$$src" $(SAVE_READER_SOURCES) $(LDFLAGS) $(ALL_LDLIBS) -o "$@"

$(WIPE): FORCE
	@base="$(WIPE_BASE)"; \
	if [ -n "$(VERSION)" ]; then src="$${base}_$(VERSION).cpp"; else src="$${base}.cpp"; fi; \
	if [ ! -f "$$src" ]; then echo "Missing source: $$src" >&2; exit 1; fi; \
	echo "$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) $$src $(WIPE_SOURCES) $(LDFLAGS) $(ALL_LDLIBS) -o $@"; \
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) "$$src" $(WIPE_SOURCES) $(LDFLAGS) $(ALL_LDLIBS) -o "$@"

check:
	@set -e; \
	for base in "$(WRITER_BASE)" "$(CARD_READER_BASE)" "$(SAVE_READER_BASE)" "$(WIPE_BASE)"; do \
		if [ -n "$(VERSION)" ]; then src="$${base}_$(VERSION).cpp"; else src="$${base}.cpp"; fi; \
		if [ ! -f "$$src" ]; then echo "Missing source: $$src" >&2; exit 1; fi; \
		echo "$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) -fsyntax-only $$src $(ALL_SUPPORT_SOURCES)"; \
		$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) -fsyntax-only "$$src" $(ALL_SUPPORT_SOURCES); \
	done

test: check
	mkdir -p build
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/arm_branch_test.cpp src/cartridge_format.cpp -o build/arm_branch_test
	./build/arm_branch_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/cartridge_format_test.cpp src/cartridge_format.cpp -o build/cartridge_format_test
	./build/cartridge_format_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/cartridge_image_builder_test.cpp src/cartridge_image_builder.cpp src/cartridge_format.cpp -o build/cartridge_image_builder_test
	./build/cartridge_image_builder_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/card_writer_test.cpp src/card_writer.cpp src/verification_policy.cpp -o build/card_writer_test
	./build/card_writer_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/ez3_catalog_test.cpp src/ez3_catalog.cpp src/cartridge_format.cpp -o build/ez3_catalog_test
	./build/ez3_catalog_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/eeprom_mapping_test.cpp src/eeprom_mapping.cpp -o build/eeprom_mapping_test
	./build/eeprom_mapping_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/card_reader_options_test.cpp src/card_reader_options.cpp -o build/card_reader_options_test
	./build/card_reader_options_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/protocol_test.cpp src/protocol.cpp src/usb_device.cpp $(ALL_LDLIBS) -o build/protocol_test
	./build/protocol_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/platform_test.cpp src/platform.cpp -o build/platform_test
	./build/platform_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/read_session_transition_test.cpp src/read_session_transition.cpp -o build/read_session_transition_test
	./build/read_session_transition_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/official_cartridge_read_test.cpp src/read_only_cartridge.cpp src/read_session_transition.cpp src/cartridge_format.cpp src/protocol.cpp src/usb_device.cpp $(ALL_LDLIBS) -o build/official_cartridge_read_test
	./build/official_cartridge_read_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/verification_policy_test.cpp src/verification_policy.cpp -o build/verification_policy_test
	./build/verification_policy_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/verification_exact_8mib_test.cpp src/verification_session.cpp src/protocol.cpp src/usb_device.cpp $(ALL_LDLIBS) -o build/verification_exact_8mib_test
	./build/verification_exact_8mib_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/verification_partial_12mib_test.cpp src/verification_session.cpp src/protocol.cpp src/usb_device.cpp $(ALL_LDLIBS) -o build/verification_partial_12mib_test
	./build/verification_partial_12mib_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/verification_exact_16mib_test.cpp src/verification_session.cpp src/protocol.cpp src/usb_device.cpp $(ALL_LDLIBS) -o build/verification_exact_16mib_test
	./build/verification_exact_16mib_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/verification_tiny_tail_above_16mib_test.cpp src/verification_session.cpp src/protocol.cpp src/usb_device.cpp $(ALL_LDLIBS) -o build/verification_tiny_tail_above_16mib_test
	./build/verification_tiny_tail_above_16mib_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/verification_partial_20mib_test.cpp src/verification_session.cpp src/protocol.cpp src/usb_device.cpp $(ALL_LDLIBS) -o build/verification_partial_20mib_test
	./build/verification_partial_20mib_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/verification_exact_24mib_test.cpp src/verification_session.cpp src/protocol.cpp src/usb_device.cpp $(ALL_LDLIBS) -o build/verification_exact_24mib_test
	./build/verification_exact_24mib_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/verification_partial_28mib_test.cpp src/verification_session.cpp src/protocol.cpp src/usb_device.cpp $(ALL_LDLIBS) -o build/verification_partial_28mib_test
	./build/verification_partial_28mib_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/verification_exact_32mib_test.cpp src/verification_session.cpp src/protocol.cpp src/usb_device.cpp $(ALL_LDLIBS) -o build/verification_exact_32mib_test
	./build/verification_exact_32mib_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/verification_partial_first_window_test.cpp src/verification_session.cpp src/protocol.cpp src/usb_device.cpp $(ALL_LDLIBS) -o build/verification_partial_first_window_test
	./build/verification_partial_first_window_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/writer_options_test.cpp src/writer_options.cpp -o build/writer_options_test
	./build/writer_options_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/version_test.cpp -o build/version_test
	./build/version_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/save_selection_test.cpp -o build/save_selection_test
	./build/save_selection_test
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) tests/save_memory_test.cpp src/save_memory_reader.cpp src/save_memory_writer.cpp src/protocol.cpp src/usb_device.cpp $(ALL_LDLIBS) -o build/save_memory_test
	./build/save_memory_test

print-config:
	@echo "VERSION=$(VERSION)"
	@echo "CXX=$(CXX)"
	@echo "CXXFLAGS=$(ALL_CXXFLAGS)"
	@echo "LIBUSB_CFLAGS=$(LIBUSB_CFLAGS)"
	@echo "LIBUSB_LIBS=$(LIBUSB_LIBS)"
	@for base in "$(WRITER_BASE)" "$(CARD_READER_BASE)" "$(SAVE_READER_BASE)" "$(WIPE_BASE)"; do \
		if [ -n "$(VERSION)" ]; then src="$${base}_$(VERSION).cpp"; else src="$${base}.cpp"; fi; \
		echo "SOURCE=$$src"; \
	done

clean:
	rm -f $(PROGRAMS) build/arm_branch_test build/cartridge_format_test build/cartridge_image_builder_test build/card_writer_test build/ez3_catalog_test build/eeprom_mapping_test build/card_reader_options_test build/protocol_test build/platform_test build/read_session_transition_test build/official_cartridge_read_test build/verification_exact_8mib_test build/verification_partial_12mib_test build/verification_exact_16mib_test build/verification_tiny_tail_above_16mib_test build/verification_partial_20mib_test build/verification_exact_24mib_test build/verification_partial_28mib_test build/verification_exact_32mib_test build/verification_policy_test build/verification_partial_first_window_test build/writer_options_test build/version_test build/save_selection_test build/save_memory_test

FORCE:
