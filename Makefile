CC = gcc
# EXTRA_CFLAGS lets release builds stamp the version, e.g.
#   make EXTRA_CFLAGS='-DMETTLE_VERSION_RAW=v0.13.0'
# (bare token, stringified in main.c - avoids fragile quote escaping)
EXTRA_CFLAGS =
CFLAGS = -Wall -Wextra -std=c99 -g -O2 -D_GNU_SOURCE -Isrc -Iinclude -fno-omit-frame-pointer $(EXTRA_CFLAGS)
# Build the driver against src/mettle_alloc.c instead of the platform heap.
# Drop this to fall back to malloc (e.g. under a sanitizer, or to attribute
# a regression). See src/mettle_alloc.h.
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
# libraries, so link them explicitly for compiler_context.c's pthread TLS and
# compiler_crash.c's dladdr. No-op on glibc >= 2.34, where libc absorbed both.
CFLAGS += -pthread
LDFLAGS = -rdynamic -pthread -ldl -lm
endif
SRCDIR = src
OBJDIR = obj
BINDIR = bin
STDLIBDIR = stdlib
RUNTIMEDIR = src/runtime

# Install prefix for `make install` / `make install-libmtlc` (honors DESTDIR).
PREFIX ?= /usr/local
# Reported by mtlc_version(); keep in sync with src/mtlc_api.c.
LIBMTLC_VERSION = 0.1.0

# Source files
LEXER_SOURCES = $(SRCDIR)/lexer/lexer.c
PARSER_SOURCES = $(SRCDIR)/parser/parser.c $(SRCDIR)/parser/ast.c
SEMANTIC_SOURCES = $(SRCDIR)/semantic/symbol_table.c $(SRCDIR)/semantic/type_checker.c $(SRCDIR)/semantic/type_checker_types.c $(SRCDIR)/semantic/type_checker_errors.c $(SRCDIR)/semantic/type_checker_safety.c $(SRCDIR)/semantic/type_checker_init_tracker.c $(SRCDIR)/semantic/type_checker_decl.c $(SRCDIR)/semantic/type_checker_match.c $(SRCDIR)/semantic/type_checker_stmt.c $(SRCDIR)/semantic/type_checker_expr.c $(SRCDIR)/semantic/type_checker_tensor_epilogue.c $(SRCDIR)/semantic/type_checker_memory.c $(SRCDIR)/semantic/register_allocator.c $(SRCDIR)/semantic/import_resolver.c $(SRCDIR)/semantic/monomorphize.c
# The AST->IR lowering pass is a FRONTEND concern (it consumes the frontend AST
# and type system), so it links into the mettle driver, not into libmtlc.
LOWERING_SOURCES = \
	$(SRCDIR)/ir/ir_lowering.c \
	$(SRCDIR)/ir/ir_lower_address.c \
	$(SRCDIR)/ir/ir_lower_defer.c \
	$(SRCDIR)/ir/ir_lower_expr.c \
	$(SRCDIR)/ir/ir_lower_stmt.c \
	$(SRCDIR)/ir/ir_lower_support.c \
	$(SRCDIR)/ir/ir_lower_switch_match.c \
	$(SRCDIR)/ir/ir_lower_types.c
# Backend IR core (everything in src/ir except the lowering TUs) + optimizer.
IR_CORE_SOURCES = $(filter-out $(LOWERING_SOURCES),$(wildcard $(SRCDIR)/ir/*.c)) $(wildcard $(SRCDIR)/ir/optimizer/*.c)
# Public libmtlc API surface, and the frontend-side type-translation adapter.
API_SOURCES = $(SRCDIR)/mtlc_api.c $(SRCDIR)/mtlc_build.c $(SRCDIR)/mtlc_lib_fallbacks.c
FRONTEND_ADAPTER_SOURCES = $(SRCDIR)/frontend/mtlc_type_from_frontend.c $(SRCDIR)/frontend/mtlc_lower_module.c
CODEGEN_SOURCES = \
	$(SRCDIR)/codegen/binary_emitter.c \
	$(SRCDIR)/codegen/code_generator.c \
	$(SRCDIR)/codegen/elf_emitter.c \
	$(SRCDIR)/codegen/ptx_emitter.c \
	$(SRCDIR)/codegen/spirv_emitter.c \
	$(wildcard $(SRCDIR)/codegen/binary/*.c)
LINKER_SOURCES = $(wildcard $(SRCDIR)/linker/*.c)
# error_reporter.c is frontend-NEUTRAL (renders against raw source text +
# SourceLocation; no AST) and the backend's comptime interpreter reports
# through it, so it belongs to libmtlc. error_explain.c stays driver-side.
DIAG_SOURCES = $(SRCDIR)/error/error_reporter.c
ERROR_SOURCES = $(SRCDIR)/error/error_explain.c
DEBUG_SOURCES = $(SRCDIR)/debug/debug_info.c
COMPILER_SOURCES = $(SRCDIR)/compiler/compiler_context.c $(SRCDIR)/compiler/compiler_crash.c
COMMON_SOURCES = $(SRCDIR)/common.c
# mettle_alloc.c interposes on malloc/free for the whole process, which is
# the host application's call to make -- so it links into the driver, not
# into libmtlc, where it would silently replace an embedder's allocator.
# Symbol resolution is global, so the backend's allocations still land in it.
MAIN_SOURCES = $(SRCDIR)/main.c $(SRCDIR)/tracy_build.c $(SRCDIR)/mettle_alloc.c

# libmtlc: the standalone, frontend-agnostic backend (IR core, optimizer + GNN,
# code generators, native linker, public API).
BACKEND_SOURCES = $(COMMON_SOURCES) $(IR_CORE_SOURCES) $(CODEGEN_SOURCES) $(LINKER_SOURCES) $(DEBUG_SOURCES) $(DIAG_SOURCES) $(COMPILER_SOURCES) $(API_SOURCES)
# The reference frontend / driver that consumes libmtlc.
FRONTEND_SOURCES = $(LEXER_SOURCES) $(PARSER_SOURCES) $(SEMANTIC_SOURCES) $(LOWERING_SOURCES) $(FRONTEND_ADAPTER_SOURCES) $(ERROR_SOURCES) $(MAIN_SOURCES)

BACKEND_OBJECTS = $(BACKEND_SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
FRONTEND_OBJECTS = $(FRONTEND_SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

AR = ar
LIBMTLC = $(BINDIR)/libmtlc.a
TARGET = $(BINDIR)/mettle

.PHONY: all clean test install install-libmtlc dist-libmtlc bundle-stdlib bundle-runtime libmtlc

all: $(TARGET) bundle-stdlib bundle-runtime
libmtlc: $(LIBMTLC)

# The static archive is the backend product, and it is self-contained: it
# references only libc and system libraries, never the Mettle frontend (the
# libmtlc_selfcontained test gate enforces that). A frontend links it alone,
# as the mettle driver and examples/calc both do.
$(LIBMTLC): $(BACKEND_OBJECTS) | $(BINDIR)
	rm -f $@
	$(AR) rcs $@ $(BACKEND_OBJECTS)

$(TARGET): $(FRONTEND_OBJECTS) $(LIBMTLC) | $(BINDIR)
	$(CC) $(FRONTEND_OBJECTS) $(LIBMTLC) -o $@ $(LDFLAGS)

bundle-stdlib: | $(BINDIR)
	rm -rf $(BINDIR)/stdlib
	cp -r $(STDLIBDIR) $(BINDIR)/stdlib

# Runtime objects are linked into every user program, so build them lean:
# no debug info (-g0 overrides the -g in CFLAGS) and one section per
# function/datum so the ELF link's --gc-sections can drop whatever a given
# program does not use.
RUNTIME_OBJ_CFLAGS = $(CFLAGS) -g0 -ffunction-sections -fdata-sections

bundle-runtime: | $(BINDIR)
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
	mkdir -p $(OBJDIR)/lexer $(OBJDIR)/parser $(OBJDIR)/semantic $(OBJDIR)/ir $(OBJDIR)/ir/optimizer $(OBJDIR)/codegen $(OBJDIR)/codegen/binary $(OBJDIR)/linker $(OBJDIR)/error $(OBJDIR)/debug $(OBJDIR)/compiler $(OBJDIR)/runtime $(OBJDIR)/frontend

$(BINDIR):
	mkdir -p $(BINDIR)

clean:
	rm -rf $(OBJDIR) $(BINDIR)

test: $(TARGET)
	@echo "Running crash handler tests..."
	$(CC) $(CFLAGS) -D_GNU_SOURCE tests/crash_handler_test.c src/runtime/crash_handler.c -Isrc -o $(BINDIR)/crash_handler_test
	@$(BINDIR)/crash_handler_test

# Install the full reference toolchain (the mettle driver + stdlib + runtime).
install: $(TARGET) bundle-stdlib bundle-runtime
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/stdlib $(DESTDIR)$(PREFIX)/runtime
	cp $(TARGET) $(DESTDIR)$(PREFIX)/bin/
	cp -r $(BINDIR)/stdlib/* $(DESTDIR)$(PREFIX)/stdlib/
	cp -r $(BINDIR)/runtime/* $(DESTDIR)$(PREFIX)/runtime/

# Install ONLY the backend for embedding: the public headers, the static
# library, and a pkg-config file. This is all a frontend developer needs
# (`cc $(pkg-config --cflags --libs libmtlc) app.c`).
install-libmtlc: $(LIBMTLC)
	mkdir -p $(DESTDIR)$(PREFIX)/include/mtlc $(DESTDIR)$(PREFIX)/lib/pkgconfig
	cp include/mtlc/*.h $(DESTDIR)$(PREFIX)/include/mtlc/
	cp $(LIBMTLC) $(DESTDIR)$(PREFIX)/lib/
	printf 'prefix=%s\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: libmtlc\nDescription: Frontend-agnostic native compiler backend\nVersion: %s\nLibs: -L$${libdir} -lmtlc\nCflags: -I$${includedir}\n' '$(PREFIX)' '$(LIBMTLC_VERSION)' > $(DESTDIR)$(PREFIX)/lib/pkgconfig/libmtlc.pc
	@echo "installed libmtlc $(LIBMTLC_VERSION) to $(DESTDIR)$(PREFIX) (include/mtlc, libmtlc.a, libmtlc.pc)"

# Stage the backend into ./dist/libmtlc (headers + lib) with no root needed:
# a self-contained folder to copy into another project or zip up.
dist-libmtlc: $(LIBMTLC)
	rm -rf dist/libmtlc
	mkdir -p dist/libmtlc/include/mtlc dist/libmtlc/lib
	cp include/mtlc/*.h dist/libmtlc/include/mtlc/
	cp $(LIBMTLC) dist/libmtlc/lib/
	@echo "staged dist/libmtlc (link with: cc -Idist/libmtlc/include app.c dist/libmtlc/lib/libmtlc.a)"

.PHONY: debug
debug: CFLAGS += -DDEBUG
debug: $(TARGET)
