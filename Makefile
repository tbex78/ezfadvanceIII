# EZF Advance III toolset Makefile
VERSION ?= 0.6.2

CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2

# Optional stricter warnings for development:
# WARNFLAGS ?= -Wall -Wextra -Wpedantic
WARNFLAGS ?=

CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=

WRITER_SRC := ezfadvanceIII_multirom_writer_$(VERSION).cpp
CARD_READER_SRC := ezfadvanceIII_card_reader_$(VERSION).cpp
SAVE_READER_SRC := ezfadvanceIII_save_reader_$(VERSION).cpp
WIPE_SRC := ezfadvanceIII_wipe_card_$(VERSION).cpp

WRITER := ezfadvanceIII_multirom_writer
CARD_READER := ezfadvanceIII_card_reader
SAVE_READER := ezfadvanceIII_save_reader
WIPE := ezfadvanceIII_wipe_card

PROGRAMS := $(WRITER) $(CARD_READER) $(SAVE_READER) $(WIPE)

UNAME_S := $(shell uname -s)
PKG_CONFIG ?= pkg-config
HAVE_PKG_CONFIG := $(shell command -v $(PKG_CONFIG) >/dev/null 2>&1 && $(PKG_CONFIG) --exists libusb-1.0 >/dev/null 2>&1 && echo yes)

ifeq ($(HAVE_PKG_CONFIG),yes)
LIBUSB_CFLAGS_RAW := $(shell $(PKG_CONFIG) --cflags libusb-1.0)
LIBUSB_CFLAGS := $(patsubst -I%,-isystem %,$(LIBUSB_CFLAGS_RAW))
LIBUSB_LIBS := $(shell $(PKG_CONFIG) --libs libusb-1.0)
else
ifeq ($(UNAME_S),Darwin)
ifneq ($(wildcard /opt/homebrew/opt/libusb/include/libusb-1.0/libusb.h),)
LIBUSB_CFLAGS := -isystem /opt/homebrew/opt/libusb/include
LIBUSB_LIBS := -L/opt/homebrew/opt/libusb/lib -lusb-1.0
else
ifneq ($(wildcard /usr/local/opt/libusb/include/libusb-1.0/libusb.h),)
LIBUSB_CFLAGS := -isystem /usr/local/opt/libusb/include
LIBUSB_LIBS := -L/usr/local/opt/libusb/lib -lusb-1.0
else
$(error libusb-1.0 not found. Install libusb with Homebrew or provide CPPFLAGS/LDFLAGS/LDLIBS manually)
endif
endif
else
LIBUSB_CFLAGS := -isystem /usr/local/include
LIBUSB_LIBS := -L/usr/local/lib -lusb-1.0
endif
endif

ALL_CXXFLAGS := $(CXXFLAGS) $(WARNFLAGS)
ALL_CPPFLAGS := $(CPPFLAGS) $(LIBUSB_CFLAGS)
ALL_LDLIBS := $(LIBUSB_LIBS) $(LDLIBS)

.PHONY: all clean writer card-reader save-reader wipe check print-config

all: $(PROGRAMS)

writer: $(WRITER)
card-reader: $(CARD_READER)
save-reader: $(SAVE_READER)
wipe: $(WIPE)

$(WRITER): $(WRITER_SRC)
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) $< $(LDFLAGS) $(ALL_LDLIBS) -o $@

$(CARD_READER): $(CARD_READER_SRC)
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) $< $(LDFLAGS) $(ALL_LDLIBS) -o $@

$(SAVE_READER): $(SAVE_READER_SRC)
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) $< $(LDFLAGS) $(ALL_LDLIBS) -o $@

$(WIPE): $(WIPE_SRC)
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) $< $(LDFLAGS) $(ALL_LDLIBS) -o $@

check:
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) -fsyntax-only $(WRITER_SRC)
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) -fsyntax-only $(CARD_READER_SRC)
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) -fsyntax-only $(SAVE_READER_SRC)
	$(CXX) $(ALL_CPPFLAGS) $(ALL_CXXFLAGS) -fsyntax-only $(WIPE_SRC)

print-config:
	@echo "VERSION=$(VERSION)"
	@echo "CXX=$(CXX)"
	@echo "UNAME_S=$(UNAME_S)"
	@echo "LIBUSB_CFLAGS=$(LIBUSB_CFLAGS)"
	@echo "LIBUSB_LIBS=$(LIBUSB_LIBS)"
	@echo "CXXFLAGS=$(ALL_CXXFLAGS)"

clean:
	rm -f $(PROGRAMS)
