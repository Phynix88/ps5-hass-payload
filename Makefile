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
          -Ithird_party/sqlite \
          -I$(MICRODNS)/include \
          -DPS5H_VERSION=\"$(VERSION)\" -DPS5H_COMMIT=\"$(COMMIT)\"
LDADD  := $(MICRODNS)/builddir/src/libmicrodns.a \
          $(MICRODNS)/builddir/compat/libcompat.a \
          -lkernel_sys -lSceSystemService -lSceUserService -lpthread

# SQLite is third-party code: its own object, without -Werror/-Wextra. The
# copy of app.db is opened immutable, so no locking, WAL or mmap is needed.
SQLITE_OBJ := build/sqlite3.o
SQLITE_DEFS := -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=0 \
               -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_DEPRECATED \
               -DSQLITE_DQS=0 -DSQLITE_MAX_MMAP_SIZE=0 \
               -DSQLITE_TEMP_STORE=3

all: $(ELF)

$(SQLITE_OBJ): third_party/sqlite/sqlite3.c third_party/sqlite/sqlite3.h Makefile
	@mkdir -p $(dir $@)
	$(CC) -Os -w $(SQLITE_DEFS) -c -o $@ third_party/sqlite/sqlite3.c

$(ELF): $(SRC) $(SQLITE_OBJ) $(wildcard src/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(SQLITE_OBJ) $(LDADD)

clean:
	rm -f $(ELF) $(SQLITE_OBJ)

.PHONY: all clean
