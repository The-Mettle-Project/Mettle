// `mettle explain <CODE>`: extended documentation for diagnostic codes,
// modeled on `rustc --explain`. One entry per stable code.
#include "error_explain.h"
#include <stdio.h>
#include <string.h>

typedef struct {
  const char *code;
  const char *title;
  const char *body;
} ErrorCodeDoc;

static const ErrorCodeDoc DOCS[] = {
    {"E0001", "Lexical error",
     "The compiler could not turn part of the source text into tokens.\n"
     "Typical causes: an unterminated string literal, an invalid numeric\n"
     "literal (e.g. 0x with no digits), or a stray character that is not\n"
     "part of the language.\n"
     "\n"
     "Example:\n"
     "    var s: string = \"unterminated;\n"
     "\n"
     "Fix: close the string, correct the literal, or delete the stray\n"
     "character. The caret in the diagnostic points at the first byte the\n"
     "lexer could not process.\n"},
    {"E0002", "Syntax error",
     "The source tokenized correctly but does not follow Mettle's grammar.\n"
     "\n"
     "Common cases and their fixes:\n"
     "  - `if`/`while`/`match` conditions require parentheses:\n"
     "        if (x > 0) { ... }        not    if x > 0 { ... }\n"
     "  - every statement ends with ';'\n"
     "  - a '{' must have a matching '}'\n"
     "  - type annotations use ':', assignment uses '=':\n"
     "        var x: int64 = 1;\n"
     "\n"
     "After a syntax error the parser resynchronizes at the next statement\n"
     "boundary and keeps going, so one mistake reports once, not as a\n"
     "cascade.\n"},
    {"E0003", "Semantic error",
     "The code is grammatically valid but does not make sense: an\n"
     "undefined variable or function, a duplicate declaration, a wrong\n"
     "argument count, `break` outside a loop, and similar.\n"
     "\n"
     "Example:\n"
     "    fn main() -> int64 {\n"
     "        return cout;      // error: Undefined variable 'cout'\n"
     "    }\n"
     "\n"
     "The compiler suggests the closest in-scope name (\"did you mean\n"
     "'count'?\") and, for duplicate declarations and call errors, points\n"
     "at the previous declaration / the function definition in a note.\n"},
    {"E0004", "Type mismatch",
     "A value of one type was used where a different type is required.\n"
     "Mettle never infers `var`/local `const` binding types and does not\n"
     "implicitly convert between most types, so both sides must line up.\n"
     "\n"
     "Example:\n"
     "    var x: int64 = \"hello\";   // expected 'int64', found 'string'\n"
     "\n"
     "Fixes:\n"
     "  - change the declared type to match the value, or the value to\n"
     "    match the type\n"
     "  - for numeric conversions, cast explicitly: (int32)value\n"},
    {"E0005", "Scope error",
     "A name was used outside the region where it is visible. Variables\n"
     "live from their declaration to the end of the enclosing block.\n"
     "\n"
     "Fix: declare the variable in a scope that encloses every use, or\n"
     "move the use into the variable's block.\n"},
    {"E0006", "I/O error",
     "The compiler could not read an input file or write an output file.\n"
     "Check that the path exists, that the file is readable, and that the\n"
     "output directory is writable. Import search paths can be extended\n"
     "with -I <dir>.\n"},
    {"E0007", "Internal compiler error",
     "The compiler itself hit a bug - this is never your program's fault.\n"
     "Re-run with --debug-compiler for a detailed report, and please file\n"
     "the reproducing source at the Mettle issue tracker.\n"},
    {"M0101", "Use after free",
     "A heap pointer is dereferenced after `free(p)` on the same path.\n"
     "The memory may already be reused; reads are garbage and writes\n"
     "corrupt other data.\n"
     "\n"
     "Fix: free last, or set the pointer aside until every use is done.\n"
     "This analysis is conservative: it only warns when the freed pointer\n"
     "and the use are provably the same allocation on the same path.\n"},
    {"M0102", "Double free",
     "`free` is called twice on the same allocation along one path, which\n"
     "corrupts the allocator's bookkeeping.\n"
     "\n"
     "Fix: free exactly once, typically at the single owner of the\n"
     "allocation. If two branches both free, hoist the free after the\n"
     "branches.\n"},
    {"M0103", "Returning the address of a stack local",
     "A function returns a pointer into its own stack frame. The frame is\n"
     "reclaimed on return, so the caller receives a dangling pointer.\n"
     "This is a hard error.\n"
     "\n"
     "Example:\n"
     "    fn bad() -> *int64 {\n"
     "        var local: int64 = 1;\n"
     "        return &local;        // M0103\n"
     "    }\n"
     "\n"
     "Fix: heap-allocate the value (`new`/`malloc`) or return it by value.\n"},
    {"M0104", "Storing a stack address in a global",
     "A pointer to a stack local is stored in a global variable. Once the\n"
     "function returns the global points at a dead frame.\n"
     "\n"
     "Fix: store heap memory in globals, or copy the value itself.\n"},
    {"M0105", "Constant array index out of bounds",
     "An array is indexed with a compile-time constant that is negative or\n"
     ">= the array length. This would always fault at runtime, so it is a\n"
     "hard error.\n"
     "\n"
     "Fix: correct the index or the array size. Remember indices are\n"
     "0-based: the last element of `var a: int64[4]` is a[3].\n"},
    {"M0106", "Memory operation overflows a stack array",
     "A memcpy/memset-style operation writes more bytes than the\n"
     "destination stack array holds, smashing adjacent stack memory.\n"
     "\n"
     "Fix: pass the correct byte count (element count * element size), or\n"
     "grow the buffer.\n"},
    {"M0107", "Memory leak",
     "An allocation never escapes the function (not returned, not stored,\n"
     "not passed on) and is never freed: the memory is unreachable after\n"
     "the function returns.\n"
     "\n"
     "Fix: free it on every path (a `defer free(p);` right after the\n"
     "allocation covers early returns), or hand ownership out.\n"},
    {"M0108", "Use after call-freed pointer",
     "A pointer is used after being passed to a function that frees it\n"
     "(directly or transitively - ownership summaries are inferred over\n"
     "the whole call graph).\n"
     "\n"
     "Fix: treat the pointer as consumed by that call; don't touch it\n"
     "afterwards.\n"},
    {"M0109", "Double free via call",
     "An allocation is freed both by a callee that takes ownership and by\n"
     "the caller (or by two consuming calls).\n"
     "\n"
     "Fix: decide which side owns the pointer and free only there.\n"},
    {"M0110", "Borrowed interior pointer outlives its scope",
     "A pointer into a stack value (a field, an array element) escapes the\n"
     "scope that owns the value, e.g. saved to an outer variable inside a\n"
     "block. When the block exits the pointee dies.\n"
     "\n"
     "Fix: shorten the pointer's lifetime to the value's scope, or move\n"
     "the value itself to the outer scope / heap.\n"},
    {"M0111", "Borrowed pointer invalidated by realloc",
     "A pointer into a heap buffer is used after the buffer was passed to\n"
     "realloc. realloc may move the allocation, leaving every old interior\n"
     "pointer dangling - even when the call \"usually\" grows in place.\n"
     "\n"
     "Fix: recompute interior pointers from the (new) base pointer after\n"
     "every realloc.\n"},
    {"M0112", "Borrowed pointer invalidated by free",
     "A pointer derived from a heap buffer (base + offset, field address)\n"
     "is used after the underlying buffer was freed.\n"
     "\n"
     "Fix: finish all uses of derived pointers before freeing the base,\n"
     "or free later.\n"},
};

static void print_code_list(void) {
  printf("Diagnostic codes:\n");
  for (size_t i = 0; i < sizeof(DOCS) / sizeof(DOCS[0]); i++) {
    printf("  %s  %s\n", DOCS[i].code, DOCS[i].title);
  }
  printf("\nRun `mettle explain <CODE>` for details on one of them.\n");
}

int mettle_explain_error_code(const char *code) {
  if (!code || strcmp(code, "list") == 0 || strcmp(code, "all") == 0) {
    print_code_list();
    return 0;
  }

  /* Accept lowercase and codes pasted with brackets: "[E0004]" */
  char normalized[16];
  size_t n = 0;
  for (const char *p = code; *p && n < sizeof(normalized) - 1; p++) {
    if (*p == '[' || *p == ']')
      continue;
    char c = *p;
    if (c >= 'a' && c <= 'z')
      c = (char)(c - 'a' + 'A');
    normalized[n++] = c;
  }
  normalized[n] = '\0';

  for (size_t i = 0; i < sizeof(DOCS) / sizeof(DOCS[0]); i++) {
    if (strcmp(DOCS[i].code, normalized) == 0) {
      printf("%s: %s\n\n%s", DOCS[i].code, DOCS[i].title, DOCS[i].body);
      return 0;
    }
  }

  fprintf(stderr, "error: unknown diagnostic code '%s'\n\n", code);
  print_code_list();
  return 1;
}
