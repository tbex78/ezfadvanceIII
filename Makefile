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
#   make VERSION=0.6.2
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
LDFLAGS =
LDLIBS =

WRITER = ezfadvanceIII_multirom_writer
CARD_READER = ezfadvanceIII_card_reader
SAVE_READER = ezfadvanceIII_save_reader
WIPE = ezfadvanceIII_wipe_card

PROGRAMS = $(WRITER) $(CARD_READER) $(SAVE_READER) $(WIPE)

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
ALL_CPPFLAGS = $(CPPFLAGS) $(LIBUSB_CFLAGS)
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
	echo "$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) $$src $(LDFLAGS) $(ALL_LDLIBS) -o $@"; \
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) "$$src" $(LDFLAGS) $(ALL_LDLIBS) -o "$@"

$(CARD_READER): FORCE
	@base="$(CARD_READER_BASE)"; \
	if [ -n "$(VERSION)" ]; then src="$${base}_$(VERSION).cpp"; else src="$${base}.cpp"; fi; \
	if [ ! -f "$$src" ]; then echo "Missing source: $$src" >&2; exit 1; fi; \
	echo "$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) $$src $(LDFLAGS) $(ALL_LDLIBS) -o $@"; \
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) "$$src" $(LDFLAGS) $(ALL_LDLIBS) -o "$@"

$(SAVE_READER): FORCE
	@base="$(SAVE_READER_BASE)"; \
	if [ -n "$(VERSION)" ]; then src="$${base}_$(VERSION).cpp"; else src="$${base}.cpp"; fi; \
	if [ ! -f "$$src" ]; then echo "Missing source: $$src" >&2; exit 1; fi; \
	echo "$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) $$src $(LDFLAGS) $(ALL_LDLIBS) -o $@"; \
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) "$$src" $(LDFLAGS) $(ALL_LDLIBS) -o "$@"

$(WIPE): FORCE
	@base="$(WIPE_BASE)"; \
	if [ -n "$(VERSION)" ]; then src="$${base}_$(VERSION).cpp"; else src="$${base}.cpp"; fi; \
	if [ ! -f "$$src" ]; then echo "Missing source: $$src" >&2; exit 1; fi; \
	echo "$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) $$src $(LDFLAGS) $(ALL_LDLIBS) -o $@"; \
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) "$$src" $(LDFLAGS) $(ALL_LDLIBS) -o "$@"

check:
	@set -e; \
	for base in "$(WRITER_BASE)" "$(CARD_READER_BASE)" "$(SAVE_READER_BASE)" "$(WIPE_BASE)"; do \
		if [ -n "$(VERSION)" ]; then src="$${base}_$(VERSION).cpp"; else src="$${base}.cpp"; fi; \
		if [ ! -f "$$src" ]; then echo "Missing source: $$src" >&2; exit 1; fi; \
		echo "$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) -fsyntax-only $$src"; \
		$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) -fsyntax-only "$$src"; \
	done

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
	rm -f $(PROGRAMS)

FORCE:
