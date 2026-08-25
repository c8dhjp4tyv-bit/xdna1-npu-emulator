# SPDX-License-Identifier: GPL-2.0-only

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wno-unused-parameter -Iinclude -Isrc
# Baslik bagimliliklarini takip et: header degisince .o yeniden derlensin.
CFLAGS  += -MMD -MP
LDFLAGS ?=

CORE_SRC := $(wildcard src/*.c)
CORE_OBJ := $(CORE_SRC:.c=.o)

BUILD    := build
TESTS    := $(BUILD)/test_boot $(BUILD)/test_exec

.PHONY: all test clean

all: $(BUILD)/libxdna.a

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/libxdna.a: $(CORE_OBJ) | $(BUILD)
	$(AR) rcs $@ $(CORE_OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

-include $(CORE_OBJ:.o=.d)

$(BUILD)/test_%: tests/test_%.c tests/drv_model.c $(BUILD)/libxdna.a | $(BUILD)
	$(CC) $(CFLAGS) -Itests $< tests/drv_model.c $(BUILD)/libxdna.a -o $@ $(LDFLAGS)

test: $(TESTS)
	@$(BUILD)/test_boot
	@echo
	@$(BUILD)/test_exec

clean:
	rm -rf $(BUILD) $(CORE_OBJ) $(CORE_OBJ:.o=.d)
