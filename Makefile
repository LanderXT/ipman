# ipman — Implementation Plan Manager
#
# Usage:
#   make                 # dev build (default)
#   make build           # alias for the default build
#   make BUILD=release   # release build (no -Werror, -O2, stripped)
#   make install         # install binary + Claude/Codex skills (see BINDIR, CLAUDE_SKILLDIR, CODEX_SKILLDIR)
#   make install-skills  # install only Claude/Codex skills
#   make test            # build then run integration tests
#   make run             # init a throwaway IPMAN_HOME and run noop
#   make clean           # remove build artifacts
#
# See docs/ipman.implementation.plan.md for the phased build-out.

CC ?= cc

CSTD      := -std=c11
WARNINGS  := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes
DEV_FLAGS := $(WARNINGS) -Werror -g -O0
REL_FLAGS := $(WARNINGS) -O2

BUILD ?= dev
ifeq ($(BUILD),release)
    CFLAGS := $(CSTD) $(REL_FLAGS)
    LDFLAGS += -s
else
    CFLAGS := $(CSTD) $(DEV_FLAGS)
endif
CFLAGS += -MMD -MP
# POSIX 2008 feature-test macro: needed under strict C11 for PATH_MAX,
# mkdir, geteuid, strftime-with-timezone, etc. to be declared.
CFLAGS += -D_POSIX_C_SOURCE=200809L

# Third-party amalgamations: compile at -O2, minimal warnings. They're not
# written to our strict warning set, and we don't want upstream churn to
# fail our build. -w suppresses warnings; we trust the pinned SHA-256.
TP_CFLAGS := $(CSTD) -O2 -g -w -MMD -MP

# SQLCipher (system-installed via libsqlcipher-dev) provides the SQLite C API
# plus transparent at-rest encryption via PRAGMA key. libsodium supplies the
# Argon2id KDF used to derive the page-encryption key.
SQLCIPHER_CFLAGS := $(shell pkg-config --cflags sqlcipher)
SQLCIPHER_LIBS   := $(shell pkg-config --libs sqlcipher)
SODIUM_CFLAGS    := $(shell pkg-config --cflags libsodium)
SODIUM_LIBS      := $(shell pkg-config --libs libsodium)

BUILD_DIR := build
GEN_DIR   := $(BUILD_DIR)/gen
BIN       := $(BUILD_DIR)/ipman
PREFIX    ?= $(HOME)/.local
BINDIR    ?= $(PREFIX)/bin
CLAUDE_SKILLDIR ?= $(HOME)/.claude/skills/ipman
CODEX_HOME      ?= $(HOME)/.codex
CODEX_SKILLDIR  ?= $(CODEX_HOME)/skills/ipman

APP_SRC := $(wildcard src/*.c)
APP_OBJ := $(APP_SRC:src/%.c=$(BUILD_DIR)/%.o)

MIG_SQL  := $(sort $(wildcard migrations/*.sql))
MIG_GEN  := $(GEN_DIR)/migrations_data.c
GEN_OBJ  := $(GEN_DIR)/migrations_data.o

SKILL_SRC := .claude/skills/ipman/SKILL.md
SKILL_GEN := $(GEN_DIR)/skill_data.c
SKILL_OBJ := $(GEN_DIR)/skill_data.o

TP_OBJ := $(BUILD_DIR)/third_party/cjson/cJSON.o

OBJ := $(APP_OBJ) $(GEN_OBJ) $(SKILL_OBJ) $(TP_OBJ)
DEP := $(OBJ:.o=.d)

# Link against system SQLCipher and libsodium. SQLCipher pulls libcrypto
# transitively for AES; libsodium provides Argon2id. libm is still needed
# by SQLite math built-ins.
LDLIBS := $(SQLCIPHER_LIBS) $(SODIUM_LIBS) -lm

UNIT_SRC := $(wildcard tests/unit/*.c)
UNIT_BIN := $(UNIT_SRC:tests/unit/%.c=$(BUILD_DIR)/tests/%)

.PHONY: all build install install-skills test unit integration run clean

all: $(BIN)

build: all

install: $(BIN)
	@mkdir -p "$(DESTDIR)$(BINDIR)"
	cp "$(BIN)" "$(DESTDIR)$(BINDIR)/ipman"
	chmod 0755 "$(DESTDIR)$(BINDIR)/ipman"
	@$(MAKE) install-skills DESTDIR="$(DESTDIR)" \
		CLAUDE_SKILLDIR="$(CLAUDE_SKILLDIR)" \
		CODEX_HOME="$(CODEX_HOME)" \
		CODEX_SKILLDIR="$(CODEX_SKILLDIR)"

install-skills:
	@mkdir -p "$(DESTDIR)$(CLAUDE_SKILLDIR)"
	cp "$(SKILL_SRC)" "$(DESTDIR)$(CLAUDE_SKILLDIR)/SKILL.md"
	@mkdir -p "$(DESTDIR)$(CODEX_SKILLDIR)"
	cp "$(SKILL_SRC)" "$(DESTDIR)$(CODEX_SKILLDIR)/SKILL.md"

$(BIN): $(OBJ)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# App source: strict flags. SQLCipher's sqlite3.h lives under
# /usr/include/sqlcipher (pkg-config emits the -I); cJSON is still vendored.
APP_INCLUDES := $(SQLCIPHER_CFLAGS) $(SODIUM_CFLAGS) -Ithird_party/cjson

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(APP_INCLUDES) -c -o $@ $<

# Generated migration data: same includes, same strict flags — this is our
# code, even though it was produced by the embed script.
$(GEN_OBJ): $(MIG_GEN)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Isrc $(APP_INCLUDES) -c -o $@ $<

$(MIG_GEN): $(MIG_SQL) scripts/embed_migrations.sh
	@mkdir -p $(@D)
	sh scripts/embed_migrations.sh $@ $(MIG_SQL)

# Generated skill data: mirror of the migration embed rule, but single-file
# and without checksums. Consumed by src/skill_install.c at runtime on init.
$(SKILL_OBJ): $(SKILL_GEN)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Isrc $(APP_INCLUDES) -c -o $@ $<

$(SKILL_GEN): $(SKILL_SRC) scripts/embed_skill.sh
	@mkdir -p $(@D)
	sh scripts/embed_skill.sh $@ $(SKILL_SRC)

# Third-party amalgamations: relaxed flags.
$(BUILD_DIR)/third_party/cjson/cJSON.o: third_party/cjson/cJSON.c
	@mkdir -p $(@D)
	$(CC) $(TP_CFLAGS) -c -o $@ $<

# Unit tests: each tests/unit/test_*.c is compiled into its own binary,
# linked against the app .o files it needs plus cJSON. The unit list is
# wildcard-driven (UNIT_BIN), so a recipe only fires for a test that has
# a corresponding source file under tests/unit/.

CLI_SELECTOR_TEST_OBJS := \
    $(BUILD_DIR)/agent_docs.o \
    $(BUILD_DIR)/cli_selector.o \
    $(BUILD_DIR)/comment_ops.o \
    $(BUILD_DIR)/context_ops.o \
    $(BUILD_DIR)/db.o \
    $(BUILD_DIR)/dispatch.o \
    $(BUILD_DIR)/env_var_ops.o \
    $(BUILD_DIR)/event_ops.o \
    $(BUILD_DIR)/export_ops.o \
    $(BUILD_DIR)/instruction_ops.o \
    $(BUILD_DIR)/ipman_home.o \
    $(BUILD_DIR)/ipman_key.o \
    $(BUILD_DIR)/json_helpers.o \
    $(BUILD_DIR)/log.o \
    $(BUILD_DIR)/migrations.o \
    $(BUILD_DIR)/phase_ops.o \
    $(BUILD_DIR)/plan_ops.o \
    $(BUILD_DIR)/project_ops.o \
    $(BUILD_DIR)/protocol.o \
    $(BUILD_DIR)/task_ops.o \
    $(BUILD_DIR)/tool_ops.o \
    $(BUILD_DIR)/validation.o \
    $(GEN_OBJ) \
    $(BUILD_DIR)/third_party/cjson/cJSON.o

$(BUILD_DIR)/tests/test_cli_selector: tests/unit/test_cli_selector.c $(CLI_SELECTOR_TEST_OBJS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(APP_INCLUDES) -Isrc -o $@ $^ $(LDLIBS)

# test_log links only log.o — log.c has no transitive deps, so we keep
# the test surface minimal and fast.
$(BUILD_DIR)/tests/test_log: tests/unit/test_log.c $(BUILD_DIR)/log.o
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Isrc -o $@ $^

unit: $(UNIT_BIN)
	@set -e; for t in $(UNIT_BIN); do \
		echo "--- $$t ---"; \
		"$$t"; \
	done

integration: all
	@set -e; for t in tests/integration/*.sh; do \
		echo "--- $$t ---"; \
		sh "$$t"; \
	done

test: unit integration

run: all
	@tmp=$$(mktemp -d); \
	IPMAN_HOME=$$tmp $(BIN) init >/dev/null; \
	echo '{"protocol_version":1,"request_id":"run","actor":"tester","op":"noop","params":{}}' \
	    | IPMAN_HOME=$$tmp $(BIN); \
	rm -rf "$$tmp"

clean:
	rm -rf $(BUILD_DIR)

-include $(DEP)
