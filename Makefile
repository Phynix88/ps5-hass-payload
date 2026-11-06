# PS5 build of ps5-hass.elf (payload SDK v0.45).
# Build only. There is deliberately no "test"/"deploy" target: sending a
# payload to the PS5 is always a deliberate, manual step.
#
#   nix-shell --run 'scripts/build-microdns.sh build/microdns && make'
#   (without Nix: see "Building" in README.md)

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined: use nix-shell, or see "Without Nix" in README.md)
endif

MICRODNS ?= build/microdns/libmicrodns
VERSION  ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
COMMIT   ?= $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)

ELF := ps5-hass.elf
SRC := $(filter-out src/platform_fake.c src/mdns_stub.c,$(wildcard src/*.c))

CFLAGS := -Wall -Wextra -Werror -O2 -g -Isrc -Ithird_party/jsmn \
          -I$(MICRODNS)/include \
          -DPS5H_VERSION=\"$(VERSION)\" -DPS5H_COMMIT=\"$(COMMIT)\"
LDADD  := $(MICRODNS)/builddir/src/libmicrodns.a \
          $(MICRODNS)/builddir/compat/libcompat.a \
          -lkernel_sys -lSceSystemService -lSceUserService -lpthread

all: $(ELF)

$(ELF): $(SRC) $(wildcard src/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDADD)

clean:
	rm -f $(ELF)

.PHONY: all clean
