# mach-boot: bootstrap compiler for mach
# builds cmach from C source

CC := clang
CFLAGS := -std=c23 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic -O2

OUT := out
BIN := $(OUT)/bin
OBJ := $(OUT)/obj

SRC_DIR := src
INC_DIR := include

SOURCES := $(shell find $(SRC_DIR) -type f -name '*.c')
OBJECTS := $(SOURCES:$(SRC_DIR)/%.c=$(OBJ)/%.o)
HEADERS := $(shell find $(INC_DIR) -type f -name '*.h')

CMACH := $(BIN)/cmach

PREFIX ?= /usr/local
DESTDIR ?=

.PHONY: all clean install uninstall

all: $(CMACH)

$(OBJ)/%.o: $(SRC_DIR)/%.c $(HEADERS) | $(OBJ)
	@echo "  cc  $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -I$(INC_DIR) -c $< -o $@

$(CMACH): $(OBJECTS) | $(BIN)
	@echo "  ld  $@"
	@$(CC) $(OBJECTS) -o $@
	@echo "cmach ready: $@"

$(BIN):
	@mkdir -p $@

$(OBJ):
	@mkdir -p $@

clean:
	@rm -rf $(OUT)

install: $(CMACH)
	@install -d $(DESTDIR)$(PREFIX)/bin
	@install -m 755 $(CMACH) $(DESTDIR)$(PREFIX)/bin/cmach
	@echo "installed cmach to $(DESTDIR)$(PREFIX)/bin/cmach"

uninstall:
	@rm -f $(DESTDIR)$(PREFIX)/bin/cmach
	@echo "uninstalled cmach from $(DESTDIR)$(PREFIX)/bin/cmach"
