# Mettle: the language, its frontend, and the driver.
#
# This builds the Mettle compiler only. The backend -- IR optimization, code
# generation, native linking -- is libmtlc, a separate project fetched by
# ./get-libmtlc.sh:
#
#     ./get-libmtlc.sh     # fetch libmtlc and build its archive
#     make                 # build bin/mettle against it
#
# Point LIBMTLC_DIR at a libmtlc checkout to build against your own copy:
#
#     make LIBMTLC_DIR=../libmtlc

CC = gcc

# Where the libmtlc dependency lives. get-libmtlc.sh unpacks it here.
LIBMTLC_DIR ?= libmtlc

# EXTRA_CFLAGS lets release builds stamp the version, e.g.
#   make EXTRA_CFLAGS='-DMETTLE_VERSION_RAW=v0.13.0'
# (bare token, stringified in main.c - avoids fragile quote escaping)
EXTRA_CFLAGS =

# -Isrc comes FIRST so this repository's headers always win over libmtlc's own
# copy of the frontend: libmtlc is a monorepo that carries the reference
# frontend too, and its src/ has to be on the include path for the backend
# headers the driver uses (ir/ir.h, ir/ir_optimize.h, codegen/*, linker/*).
# docs/mettle-and-libmtlc.md describes that boundary.
CFLAGS = -Wall -Wextra -std=c99 -g -O2 -D_GNU_SOURCE \
         -Isrc -I$(LIBMTLC_DIR)/include -I$(LIBMTLC_DIR)/src \
         -fno-omit-frame-pointer $(EXTRA_CFLAGS)

# Build the driver against src/mettle_alloc.c instead of the platform heap.
# Drop this to fall back to malloc (e.g. to attribute a regression). A
# sanitizer build needs no flag: the allocator detects one and stands down,
# because ASan/TSan/MSan/LSan intercept malloc themselves. See
# src/mettle_alloc.h.
INTERNAL_ALLOC ?= 1
ifeq ($(INTERNAL_ALLOC),1)
CFLAGS += -DMETTLE_INTERNAL_ALLOC
endif

# Native compiler build profile for DGX Spark. GCC/Clang versions without a
# GB10-specific scheduler use ARMv9.2-A; GCC 15 / LLVM 21 users should override
# with `DGX_SPARK_CFLAGS=-mcpu=gb10` as recommended by NVIDIA.
DGX_SPARK ?= 0
DGX_SPARK_CFLAGS ?= -march=armv9.2-a
ifeq ($(DGX_SPARK),1)
CFLAGS += $(DGX_SPARK_CFLAGS)
endif

LDFLAGS =
ifneq ($(filter Linux linux-gnu,$(shell uname -s 2>/dev/null)),)
# glibc < 2.34 (e.g. Rocky 8 / glibc 2.28) ships pthread + dl as separate
# libraries, so link them explicitly for libmtlc's pthread TLS and its crash
# reporter's dladdr. No-op on glibc >= 2.34, where libc absorbed both.
CFLAGS += -pthread
LDFLAGS = -rdynamic -pthread -ldl -lm
endif

SRCDIR = src
OBJDIR = obj
BINDIR = bin
STDLIBDIR = stdlib
RUNTIMEDIR = src/runtime

# Install prefix for `make install` (honors DESTDIR).
PREFIX ?= /usr/local

# ---------------------------------------------------------------------------
# The libmtlc dependency.
#
# A dist bundle drops the archive in lib/; building from source puts it in
# bin/, which is what get-libmtlc.sh produces. Prefer a prebuilt one, and
# otherwise delegate to libmtlc's own Makefile, so the backend's object list
# lives in exactly one place.
# ---------------------------------------------------------------------------
LIBMTLC_PREBUILT := $(firstword $(wildcard $(LIBMTLC_DIR)/lib/libmtlc.a \
                                           $(LIBMTLC_DIR)/lib/mtlc.lib))
ifeq ($(LIBMTLC_PREBUILT),)
LIBMTLC_ARCHIVE = $(LIBMTLC_DIR)/bin/libmtlc.a
else
LIBMTLC_ARCHIVE = $(LIBMTLC_PREBUILT)
endif

ifeq ($(filter clean help,$(MAKECMDGOALS)),)
ifeq ($(wildcard $(LIBMTLC_DIR)/include/mtlc/mtlc.h),)
$(error libmtlc not found in '$(LIBMTLC_DIR)'. Run ./get-libmtlc.sh, or set \
LIBMTLC_DIR to a libmtlc checkout -- see https://github.com/The-Mettle-Project/libmtlc)
endif
endif

# ---------------------------------------------------------------------------
# The Mettle frontend. Every source file listed here is this repository's;
# everything else the compiler needs comes out of the libmtlc archive.
# ---------------------------------------------------------------------------
LEXER_SOURCES = $(SRCDIR)/lexer/lexer.c
PARSER_SOURCES = $(SRCDIR)/parser/parser.c $(SRCDIR)/parser/ast.c
SEMANTIC_SOURCES = $(SRCDIR)/semantic/symbol_table.c $(SRCDIR)/semantic/type_checker.c $(SRCDIR)/semantic/type_checker_types.c $(SRCDIR)/semantic/type_checker_errors.c $(SRCDIR)/semantic/type_checker_safety.c $(SRCDIR)/semantic/type_checker_init_tracker.c $(SRCDIR)/semantic/type_checker_decl.c $(SRCDIR)/semantic/type_checker_match.c $(SRCDIR)/semantic/type_checker_stmt.c $(SRCDIR)/semantic/type_checker_expr.c $(SRCDIR)/semantic/type_checker_aggregate.c $(SRCDIR)/semantic/type_checker_tensor_epilogue.c $(SRCDIR)/semantic/type_checker_memory.c $(SRCDIR)/semantic/register_allocator.c $(SRCDIR)/semantic/import_resolver.c $(SRCDIR)/semantic/monomorphize.c
# AST->IR lowering: a frontend pass. It consumes the AST and the frontend type
# system and emits libmtlc IR, so it lives here rather than in the backend.
LOWERING_SOURCES = \
	$(SRCDIR)/ir/ir_lowering.c \
	$(SRCDIR)/ir/ir_lower_address.c \
	$(SRCDIR)/ir/ir_lower_defer.c \
	$(SRCDIR)/ir/ir_lower_expr.c \
	$(SRCDIR)/ir/ir_lower_stmt.c \
	$(SRCDIR)/ir/ir_lower_support.c \
	$(SRCDIR)/ir/ir_lower_switch_match.c \
	$(SRCDIR)/ir/ir_lower_types.c
# Adapters from the frontend's own types and symbols onto libmtlc's.
FRONTEND_ADAPTER_SOURCES = $(SRCDIR)/frontend/mtlc_type_from_frontend.c $(SRCDIR)/frontend/mtlc_lower_module.c
# The --explain renderer. libmtlc owns error_reporter.c, which is
# frontend-neutral; this is the Mettle-specific optimization report.
ERROR_SOURCES = $(SRCDIR)/error/error_explain.c
# mettle_alloc.c interposes on malloc/free for the whole process, which is the
# host application's call to make -- so it is the driver's, not the library's.
# Symbol resolution is global, so libmtlc's allocations land in it too.
MAIN_SOURCES = $(SRCDIR)/main.c $(SRCDIR)/tracy_build.c $(SRCDIR)/mettle_alloc.c

FRONTEND_SOURCES = $(LEXER_SOURCES) $(PARSER_SOURCES) $(SEMANTIC_SOURCES) $(LOWERING_SOURCES) $(FRONTEND_ADAPTER_SOURCES) $(ERROR_SOURCES) $(MAIN_SOURCES)
FRONTEND_OBJECTS = $(FRONTEND_SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

TARGET = $(BINDIR)/mettle

.PHONY: all clean test install libmtlc bundle-stdlib bundle-runtime help debug

all: $(TARGET) bundle-stdlib bundle-runtime

# Build (or rebuild) the dependency's archive through libmtlc's own Makefile.
libmtlc:
	$(MAKE) -C $(LIBMTLC_DIR) libmtlc

$(LIBMTLC_DIR)/bin/libmtlc.a:
	@echo "Building the libmtlc archive in $(LIBMTLC_DIR)..."
	$(MAKE) -C $(LIBMTLC_DIR) libmtlc

$(TARGET): $(FRONTEND_OBJECTS) $(LIBMTLC_ARCHIVE) | $(BINDIR)
	$(CC) $(FRONTEND_OBJECTS) $(LIBMTLC_ARCHIVE) -o $@ $(LDFLAGS)

bundle-stdlib: | $(BINDIR)
	rm -rf $(BINDIR)/stdlib
	cp -r $(STDLIBDIR) $(BINDIR)/stdlib

# Runtime objects are linked into every user program, so build them lean:
# no debug info (-g0 overrides the -g in CFLAGS) and one section per
# function/datum so the ELF link's --gc-sections can drop whatever a given
# program does not use.
RUNTIME_OBJ_CFLAGS = $(CFLAGS) -g0 -ffunction-sections -fdata-sections

bundle-runtime: | $(BINDIR) $(OBJDIR)
	rm -rf $(BINDIR)/runtime
	cp -r $(RUNTIMEDIR) $(BINDIR)/runtime
	$(CC) $(RUNTIME_OBJ_CFLAGS) -c $(STDLIBDIR)/tracy_helpers.c -o $(OBJDIR)/runtime/tracy_helpers.o
	cp $(OBJDIR)/runtime/tracy_helpers.o $(BINDIR)/runtime/tracy_helpers.o
	cp $(OBJDIR)/runtime/tracy_helpers.o $(BINDIR)/runtime/tracy_helpers.obj
	$(CC) $(RUNTIME_OBJ_CFLAGS) -c $(RUNTIMEDIR)/atomics.c       -o $(OBJDIR)/runtime/atomics.o
	$(CC) $(RUNTIME_OBJ_CFLAGS) -c $(RUNTIMEDIR)/crash_handler.c -o $(OBJDIR)/runtime/crash_handler.o
	$(CC) $(RUNTIME_OBJ_CFLAGS) -c $(RUNTIMEDIR)/profile.c       -o $(OBJDIR)/runtime/profile.o
	$(CC) $(RUNTIME_OBJ_CFLAGS) -c $(RUNTIMEDIR)/posix_helpers.c -o $(OBJDIR)/runtime/posix_helpers.o
	cp $(OBJDIR)/runtime/atomics.o       $(BINDIR)/runtime/atomics.o
	cp $(OBJDIR)/runtime/crash_handler.o $(BINDIR)/runtime/crash_handler.o
	cp $(OBJDIR)/runtime/profile.o       $(BINDIR)/runtime/profile.o
	cp $(OBJDIR)/runtime/posix_helpers.o $(BINDIR)/runtime/posix_helpers.o

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)/lexer $(OBJDIR)/parser $(OBJDIR)/semantic $(OBJDIR)/ir $(OBJDIR)/error $(OBJDIR)/runtime $(OBJDIR)/frontend

$(BINDIR):
	mkdir -p $(BINDIR)

# Only this repository's build products. Clean the dependency with
# `make -C $(LIBMTLC_DIR) clean`, or delete $(LIBMTLC_DIR) and re-fetch.
clean:
	rm -rf $(OBJDIR) $(BINDIR)

test: $(TARGET)
	@echo "Running crash handler tests..."
	$(CC) $(CFLAGS) -D_GNU_SOURCE tests/crash_handler_test.c src/runtime/crash_handler.c -Isrc -o $(BINDIR)/crash_handler_test
	@$(BINDIR)/crash_handler_test

install: $(TARGET) bundle-stdlib bundle-runtime
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/stdlib $(DESTDIR)$(PREFIX)/runtime
	cp $(TARGET) $(DESTDIR)$(PREFIX)/bin/
	cp -r $(BINDIR)/stdlib/* $(DESTDIR)$(PREFIX)/stdlib/
	cp -r $(BINDIR)/runtime/* $(DESTDIR)$(PREFIX)/runtime/

help:
	@echo "Mettle -- the language, its frontend, and the driver."
	@echo ""
	@echo "  ./get-libmtlc.sh    fetch the libmtlc backend and build its archive"
	@echo "  make                build bin/mettle plus the bundled stdlib and runtime"
	@echo "  make libmtlc        rebuild the dependency's archive"
	@echo "  make test           run the C-level tests"
	@echo "  make install        install to \$$PREFIX (default /usr/local)"
	@echo "  make clean          remove this repository's build products"
	@echo ""
	@echo "  LIBMTLC_DIR=<path>  build against a libmtlc checkout (default ./libmtlc)"

debug: CFLAGS += -DDEBUG
debug: $(TARGET)
