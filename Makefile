# SPDX-License-Identifier: GPL-2.0-only

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wno-unused-parameter -Iinclude -Isrc
LDFLAGS ?=

CORE_SRC := $(wildcard src/*.c)
CORE_OBJ := $(CORE_SRC:.c=.o)

BUILD    := build
TESTS    := $(BUILD)/test_boot
QEMU_SCRIPT := scripts/build-qemu.sh

.PHONY: all test qemu clean

all: $(BUILD)/libxdna.a

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/libxdna.a: $(CORE_OBJ) | $(BUILD)
	$(AR) rcs $@ $(CORE_OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/test_boot: tests/test_boot.c $(BUILD)/libxdna.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libxdna.a -o $@ $(LDFLAGS)

test: $(TESTS)
	@$(BUILD)/test_boot

qemu:
	@$(QEMU_SCRIPT)

clean:
	rm -rf $(BUILD) $(CORE_OBJ)
