# hola-cc — the Hola agent language: holacc compiler and its runtime.
# Linux (x86_64) only for now. Needs flex, bison and clang (LLVM).
#
# The compiler and the core runtime stand alone. The agent runtime builds
# against the Hola ADK (HOLA_ADK_DIR): Hola's public headers and prebuilt
# libhola.a / libhola_tools.a, made by scripts/build-adk.sh in the Hola repo.
# Without it, `make` builds the compiler and core runtime only, and programs
# that declare agents cannot link.

CC       ?= cc
CLANG    ?= clang
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
HOLA_ADK_DIR ?=
BUILD    := build
VERSION  := $(shell cat VERSION)
PREFIX   ?= $(HOME)/.local
DIST_NAME := hola-agent-toolchain-$(VERSION)-linux-x86_64
DIST_DIR  := dist/$(DIST_NAME)
RT_DIR   := $(abspath $(BUILD))

COMPILER_SRCS := src/main.c src/gen.c src/ast.c src/strbuf.c
COMPILER_OBJS := $(BUILD)/main.o $(BUILD)/gen.o $(BUILD)/ast.o $(BUILD)/strbuf.o \
                 $(BUILD)/parser.o $(BUILD)/lexer.o $(BUILD)/cJSON.o
HAVE_ADK      := $(if $(HOLA_ADK_DIR),$(wildcard $(HOLA_ADK_DIR)/lib/libhola.a))

RT_CFLAGS := -O2 -g -fPIC -Wall -Wextra -Wno-unused-parameter -Iruntime -Ithird_party/cjson

.PHONY: all clean test test-lang test-e2e dist install uninstall

ALL := $(BUILD)/holacc $(BUILD)/libholart.a $(BUILD)/libcjson.a
ifneq ($(HAVE_ADK),)
ALL += $(BUILD)/libholart_agent.a
endif

all: $(ALL)
ifeq ($(HAVE_ADK),)
	@echo "note: no Hola ADK (set HOLA_ADK_DIR); built the compiler and core runtime only"
endif

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/parser.c $(BUILD)/parser.h: src/parser.y | $(BUILD)
	bison -Wall -d -o $(BUILD)/parser.c src/parser.y

$(BUILD)/lexer.c: src/lexer.l $(BUILD)/parser.h | $(BUILD)
	flex -o $@ src/lexer.l

$(BUILD)/prelude.h: lib/prelude.hl | $(BUILD)
	{ echo 'static const char k_prelude[] ='; \
	  sed 's/\\/\\\\/g; s/"/\\"/g; s/^/"/; s/$$/\\n"/' lib/prelude.hl; \
	  echo ';'; } > $@

$(BUILD)/parser.o: $(BUILD)/parser.c
	$(CC) $(CFLAGS) -Wno-unused-function -Isrc -I$(BUILD) -c $< -o $@

$(BUILD)/lexer.o: $(BUILD)/lexer.c
	$(CC) $(CFLAGS) -Wno-unused-function -Isrc -I$(BUILD) -c $< -o $@

$(BUILD)/main.o: src/main.c $(BUILD)/prelude.h src/ast.h src/gen.h src/strbuf.h
	$(CC) $(CFLAGS) -Isrc -I$(BUILD) -DHOLACC_RUNTIME_DIR='"$(RT_DIR)"' \
	    -DHOLA_ADK_DIR='"$(abspath $(HOLA_ADK_DIR))"' -DHOLACC_VERSION='"$(VERSION)"' -c $< -o $@

$(BUILD)/%.o: src/%.c src/ast.h src/gen.h src/strbuf.h runtime/chacha20.h $(BUILD)/parser.h
	$(CC) $(CFLAGS) -Isrc -I$(BUILD) -Iruntime -Ithird_party/cjson -c $< -o $@

$(BUILD)/holacc: $(COMPILER_OBJS)
	$(CC) $(CFLAGS) -o $@ $(COMPILER_OBJS)

# ---- runtime ----------------------------------------------------------

$(BUILD)/rt_core.o: runtime/rt_core.c runtime/hl_rt.h runtime/chacha20.h | $(BUILD)
	$(CC) $(RT_CFLAGS) -c $< -o $@

$(BUILD)/libholart.a: $(BUILD)/rt_core.o
	ar rcs $@ $^

# cJSON is also inside libhola.a; programs with agents take it from there
# (the linker only pulls this copy when nothing else defines it).
$(BUILD)/cJSON.o: third_party/cjson/cJSON.c | $(BUILD)
	$(CC) -O2 -fPIC -c $< -o $@

$(BUILD)/libcjson.a: $(BUILD)/cJSON.o
	ar rcs $@ $^

$(BUILD)/rt_agent.o: runtime/rt_agent.c runtime/hl_rt.h | $(BUILD)
	$(CC) $(RT_CFLAGS) -I$(HOLA_ADK_DIR)/include -c $< -o $@

$(BUILD)/libholart_agent.a: $(BUILD)/rt_agent.o
	ar rcs $@ $^

# ---- tests ------------------------------------------------------------

test: test-lang test-e2e

test-lang: all
	./tests/run_lang.sh

test-e2e: all
	./tests/e2e/run_e2e.sh

# ---- the Hola Agent Toolchain -------------------------------------------
#
#   <prefix>/bin/holacc
#   <prefix>/lib/holacc/       libholart.a libholart_agent.a libcjson.a
#   <prefix>/lib/hola-adk/     the Hola ADK (headers, libhola.a, libhola_tools.a)
#   <prefix>/share/hola-agent-toolchain/   README, examples, LICENSE

dist: all
	@test -n "$(HAVE_ADK)" || { echo "dist needs the Hola ADK: make dist HOLA_ADK_DIR=..."; exit 1; }
	rm -rf $(DIST_DIR)
	$(MAKE) --no-print-directory install PREFIX=$(abspath $(DIST_DIR)) HOLA_ADK_DIR=$(HOLA_ADK_DIR)
	cp scripts/install.sh $(DIST_DIR)/install.sh
	cp VERSION LICENSE $(DIST_DIR)/
	tar -C dist -czf dist/$(DIST_NAME).tar.gz $(DIST_NAME)
	@echo dist/$(DIST_NAME).tar.gz

install: all
	@test -n "$(HAVE_ADK)" || { echo "install needs the Hola ADK: make install HOLA_ADK_DIR=..."; exit 1; }
	mkdir -p $(PREFIX)/bin $(PREFIX)/lib/holacc $(PREFIX)/lib/hola-adk $(PREFIX)/share/hola-agent-toolchain
	install -m 755 $(BUILD)/holacc $(PREFIX)/bin/holacc
	install -m 644 $(BUILD)/libholart.a $(BUILD)/libholart_agent.a $(BUILD)/libcjson.a $(PREFIX)/lib/holacc/
	cp -R $(HOLA_ADK_DIR)/. $(PREFIX)/lib/hola-adk/
	cp -R README.md LICENSE examples $(PREFIX)/share/hola-agent-toolchain/

uninstall:
	rm -rf $(PREFIX)/bin/holacc $(PREFIX)/lib/holacc $(PREFIX)/lib/hola-adk $(PREFIX)/share/hola-agent-toolchain

clean:
	rm -rf $(BUILD) dist
