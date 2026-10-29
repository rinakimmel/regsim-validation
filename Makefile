# Makefile — regsim project
#
# Targets:
#   all         Build server, regctl, and all test binaries (release)
#   asan        Build everything with AddressSanitizer + UBSan
#   test        Run unit tests (no server required)
#   clean       Remove all build artefacts
#
# Build output:
#   build/release/   release binaries
#   build/asan/      sanitizer binaries

CC      := gcc
STD     := -std=c11
WARN    := -Wall -Wextra -Wpedantic -Werror
OPT     := -O2
DBGINFO := -g

# Feature test macros for Linux/POSIX APIs
DEFS    := -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L

INCLUDES := -Iinclude

# Common CFLAGS for all targets
BASE_CFLAGS := $(STD) $(WARN) $(OPT) $(DBGINFO) $(DEFS) $(INCLUDES)

# Release flags
REL_CFLAGS := $(BASE_CFLAGS)

# ASan + UBSan flags
ASAN_CFLAGS := $(STD) $(WARN) -O1 $(DBGINFO) $(DEFS) $(INCLUDES) \
               -fsanitize=address,undefined \
               -fno-sanitize-recover=all \
               -fno-omit-frame-pointer

# Linker flags
LDFLAGS_COMMON := -lpthread
REL_LDFLAGS  := $(LDFLAGS_COMMON)
ASAN_LDFLAGS := $(LDFLAGS_COMMON) -fsanitize=address,undefined

# -------------------------------------------------------------------------
# Source files
# -------------------------------------------------------------------------

# Core library objects (shared by server, client, and tests)
LIB_SRCS := src/crc32.c src/protocol.c src/regfile.c src/log.c

# Server objects
SERVER_SRCS := $(LIB_SRCS) src/server.c src/server_main.c

# Client library + CLI
CLIENT_LIB_SRCS := $(LIB_SRCS) src/client.c
REGCTL_SRCS     := $(CLIENT_LIB_SRCS) src/regctl.c

# Test binaries
TEST_CODEC_SRCS   := $(LIB_SRCS) tests/test_codec.c
TEST_REGFILE_SRCS := $(LIB_SRCS) tests/test_regfile.c
VALIDATE_SRCS     := $(LIB_SRCS) src/client.c tests/validate.c

# -------------------------------------------------------------------------
# Build directories
# -------------------------------------------------------------------------

REL_DIR  := build/release
ASAN_DIR := build/asan

# -------------------------------------------------------------------------
# Generate object file lists
# -------------------------------------------------------------------------

rel_obj  = $(patsubst %.c,$(REL_DIR)/%.o,$(1))
asan_obj = $(patsubst %.c,$(ASAN_DIR)/%.o,$(1))

REL_SERVER_OBJS   := $(call rel_obj,$(SERVER_SRCS))
REL_REGCTL_OBJS   := $(call rel_obj,$(REGCTL_SRCS))
REL_CODEC_OBJS    := $(call rel_obj,$(TEST_CODEC_SRCS))
REL_REGFILE_OBJS  := $(call rel_obj,$(TEST_REGFILE_SRCS))
REL_VALIDATE_OBJS := $(call rel_obj,$(VALIDATE_SRCS))

ASAN_SERVER_OBJS   := $(call asan_obj,$(SERVER_SRCS))
ASAN_REGCTL_OBJS   := $(call asan_obj,$(REGCTL_SRCS))
ASAN_CODEC_OBJS    := $(call asan_obj,$(TEST_CODEC_SRCS))
ASAN_REGFILE_OBJS  := $(call asan_obj,$(TEST_REGFILE_SRCS))
ASAN_VALIDATE_OBJS := $(call asan_obj,$(VALIDATE_SRCS))

# -------------------------------------------------------------------------
# Phony targets
# -------------------------------------------------------------------------

.PHONY: all asan test clean dirs

all: dirs \
     $(REL_DIR)/regsim-server \
     $(REL_DIR)/regctl \
     $(REL_DIR)/test_codec \
     $(REL_DIR)/test_regfile \
     $(REL_DIR)/validate

asan: dirs \
      $(ASAN_DIR)/regsim-server \
      $(ASAN_DIR)/regctl \
      $(ASAN_DIR)/test_codec \
      $(ASAN_DIR)/test_regfile \
      $(ASAN_DIR)/validate

test: all
	@echo "Running test_codec..."
	$(REL_DIR)/test_codec
	@echo ""
	@echo "Running test_regfile..."
	$(REL_DIR)/test_regfile

dirs:
	@mkdir -p $(REL_DIR)/src $(REL_DIR)/tests \
	           $(ASAN_DIR)/src $(ASAN_DIR)/tests

# -------------------------------------------------------------------------
# Release: compile rules
# -------------------------------------------------------------------------

$(REL_DIR)/src/%.o: src/%.c
	$(CC) $(REL_CFLAGS) -c $< -o $@

$(REL_DIR)/tests/%.o: tests/%.c
	$(CC) $(REL_CFLAGS) -c $< -o $@

# -------------------------------------------------------------------------
# Release: link rules
# -------------------------------------------------------------------------

$(REL_DIR)/regsim-server: $(REL_SERVER_OBJS)
	$(CC) $(REL_CFLAGS) $^ -o $@ $(REL_LDFLAGS)

$(REL_DIR)/regctl: $(REL_REGCTL_OBJS)
	$(CC) $(REL_CFLAGS) $^ -o $@ $(REL_LDFLAGS)

$(REL_DIR)/test_codec: $(REL_CODEC_OBJS)
	$(CC) $(REL_CFLAGS) $^ -o $@ $(REL_LDFLAGS)

$(REL_DIR)/test_regfile: $(REL_REGFILE_OBJS)
	$(CC) $(REL_CFLAGS) $^ -o $@ $(REL_LDFLAGS)

$(REL_DIR)/validate: $(REL_VALIDATE_OBJS)
	$(CC) $(REL_CFLAGS) $^ -o $@ $(REL_LDFLAGS)

# -------------------------------------------------------------------------
# ASan: compile rules
# -------------------------------------------------------------------------

$(ASAN_DIR)/src/%.o: src/%.c
	$(CC) $(ASAN_CFLAGS) -c $< -o $@

$(ASAN_DIR)/tests/%.o: tests/%.c
	$(CC) $(ASAN_CFLAGS) -c $< -o $@

# -------------------------------------------------------------------------
# ASan: link rules
# -------------------------------------------------------------------------

$(ASAN_DIR)/regsim-server: $(ASAN_SERVER_OBJS)
	$(CC) $(ASAN_CFLAGS) $^ -o $@ $(ASAN_LDFLAGS)

$(ASAN_DIR)/regctl: $(ASAN_REGCTL_OBJS)
	$(CC) $(ASAN_CFLAGS) $^ -o $@ $(ASAN_LDFLAGS)

$(ASAN_DIR)/test_codec: $(ASAN_CODEC_OBJS)
	$(CC) $(ASAN_CFLAGS) $^ -o $@ $(ASAN_LDFLAGS)

$(ASAN_DIR)/test_regfile: $(ASAN_REGFILE_OBJS)
	$(CC) $(ASAN_CFLAGS) $^ -o $@ $(ASAN_LDFLAGS)

$(ASAN_DIR)/validate: $(ASAN_VALIDATE_OBJS)
	$(CC) $(ASAN_CFLAGS) $^ -o $@ $(ASAN_LDFLAGS)

# -------------------------------------------------------------------------
# Clean
# -------------------------------------------------------------------------

clean:
	rm -rf build/
