CC = gcc
# EXTRA_CFLAGS lets release builds stamp the version, e.g.
#   make EXTRA_CFLAGS='-DMETTLE_VERSION_RAW=v0.13.0'
# (bare token, stringified in main.c - avoids fragile quote escaping)
EXTRA_CFLAGS =
CFLAGS = -Wall -Wextra -std=c99 -g -O2 -D_GNU_SOURCE -Isrc -fno-omit-frame-pointer $(EXTRA_CFLAGS)
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

# Source files
LEXER_SOURCES = $(SRCDIR)/lexer/lexer.c
PARSER_SOURCES = $(SRCDIR)/parser/parser.c $(SRCDIR)/parser/ast.c
SEMANTIC_SOURCES = $(SRCDIR)/semantic/symbol_table.c $(SRCDIR)/semantic/type_checker.c $(SRCDIR)/semantic/type_checker_types.c $(SRCDIR)/semantic/type_checker_errors.c $(SRCDIR)/semantic/type_checker_safety.c $(SRCDIR)/semantic/type_checker_init_tracker.c $(SRCDIR)/semantic/type_checker_decl.c $(SRCDIR)/semantic/type_checker_match.c $(SRCDIR)/semantic/type_checker_stmt.c $(SRCDIR)/semantic/type_checker_expr.c $(SRCDIR)/semantic/type_checker_memory.c $(SRCDIR)/semantic/register_allocator.c $(SRCDIR)/semantic/import_resolver.c $(SRCDIR)/semantic/monomorphize.c
IR_SOURCES = $(wildcard $(SRCDIR)/ir/*.c) $(wildcard $(SRCDIR)/ir/optimizer/*.c)
CODEGEN_SOURCES = \
	$(SRCDIR)/codegen/binary_emitter.c \
	$(SRCDIR)/codegen/code_generator.c \
	$(SRCDIR)/codegen/elf_emitter.c \
	$(SRCDIR)/codegen/program_entry.c \
	$(SRCDIR)/codegen/ptx_emitter.c \
	$(wildcard $(SRCDIR)/codegen/binary/*.c)
LINKER_SOURCES = $(wildcard $(SRCDIR)/linker/*.c)
ERROR_SOURCES = $(SRCDIR)/error/error_reporter.c $(SRCDIR)/error/error_explain.c
DEBUG_SOURCES = $(SRCDIR)/debug/debug_info.c
COMPILER_SOURCES = $(SRCDIR)/compiler/compiler_context.c $(SRCDIR)/compiler/compiler_crash.c
COMMON_SOURCES = $(SRCDIR)/common.c
MAIN_SOURCES = $(SRCDIR)/main.c $(SRCDIR)/tracy_build.c

SOURCES = $(COMMON_SOURCES) $(LEXER_SOURCES) $(PARSER_SOURCES) $(SEMANTIC_SOURCES) $(IR_SOURCES) $(CODEGEN_SOURCES) $(LINKER_SOURCES) $(ERROR_SOURCES) $(DEBUG_SOURCES) $(COMPILER_SOURCES) $(MAIN_SOURCES)
OBJECTS = $(SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

TARGET = $(BINDIR)/mettle

.PHONY: all clean test install bundle-stdlib bundle-runtime

all: $(TARGET) bundle-stdlib bundle-runtime

$(TARGET): $(OBJECTS) | $(BINDIR)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

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
	mkdir -p $(OBJDIR)/lexer $(OBJDIR)/parser $(OBJDIR)/semantic $(OBJDIR)/ir $(OBJDIR)/ir/optimizer $(OBJDIR)/codegen $(OBJDIR)/codegen/binary $(OBJDIR)/linker $(OBJDIR)/error $(OBJDIR)/debug $(OBJDIR)/compiler $(OBJDIR)/runtime

$(BINDIR):
	mkdir -p $(BINDIR)

clean:
	rm -rf $(OBJDIR) $(BINDIR)

test: $(TARGET)
	@echo "Running crash handler tests..."
	$(CC) $(CFLAGS) -D_GNU_SOURCE tests/crash_handler_test.c src/runtime/crash_handler.c -Isrc -o $(BINDIR)/crash_handler_test
	@$(BINDIR)/crash_handler_test

install: $(TARGET) bundle-stdlib bundle-runtime
	mkdir -p /usr/local/bin /usr/local/stdlib /usr/local/runtime
	cp $(TARGET) /usr/local/bin/
	cp -r $(BINDIR)/stdlib/* /usr/local/stdlib/
	cp -r $(BINDIR)/runtime/* /usr/local/runtime/

.PHONY: debug
debug: CFLAGS += -DDEBUG
debug: $(TARGET)
