/**
 * Mettle Extension
 *
 * Two diagnostic layers:
 * 1. Regex-based: invalid literals and unterminated comments/strings (instant)
 * 2. Compiler-backed: run Mettle for real semantic errors (type, scope, etc.)
 *
 * Compiler diagnostics run on open, save, and when the extension activates with a .mettle file open.
 * Regex runs on every edit (debounced). Regex-only passes merge with cached compiler diagnostics
 * (same document version) so they cannot wipe compiler results after save or focus changes.
 */

const vscode = require('vscode');
const path = require('path');
const { execFile } = require('child_process');
const fs = require('fs');
const os = require('os');
const { registerExplain } = require('./explain');
const { registerLanguageFeatures } = require('./language');
const { registerCodegen } = require('./codegen');
const { registerProfile } = require('./profile');
const { registerDebugAdapter } = require('./debugAdapter');

/** @type {vscode.DiagnosticCollection} */
let diagnosticCollection;
/** @type {vscode.OutputChannel | null} */
let mettleOutputChannel = null;
/** @type {Map<string, NodeJS.Timeout>} */
const debounceTimers = new Map();
const DEBOUNCE_MS = 150;

/** Cached compiler diagnostics: invalidated when document version changes (any edit). */
/** @type {Map<string, { version: number, diagnostics: vscode.Diagnostic[] }>} */
const compilerDiagnosticsCache = new Map();
/** URIs currently running mettle (regex-only updates skipped to avoid wiping results mid-compile). */
/** @type {Set<string>} */
const compilingUriKeys = new Set();

const METTLE_SELECTOR = { language: 'mettle' };

const TYPE_SIZES = {
  int8: '1 byte, signed integer',
  uint8: '1 byte, unsigned integer',
  int16: '2 bytes, signed integer',
  uint16: '2 bytes, unsigned integer',
  int32: '4 bytes, signed integer; default integer literal type',
  uint32: '4 bytes, unsigned integer',
  int64: '8 bytes, signed integer; pointer-sized on x86-64',
  uint64: '8 bytes, unsigned integer',
  float32: '4 bytes, IEEE 754 float',
  float64: '8 bytes, IEEE 754 float; default floating literal type',
  bool: 'Boolean-like integer value; comparisons produce 0 or 1',
  void: 'No value',
};

const HOVER_DOCS = new Map(Object.entries({
  import: {
    title: 'import',
    body: [
      'Includes another `.mettle` module at compile time. Imports are resolved transitively from the single entry file, then flattened into one program.',
      'If the imported module uses `export`, only exported declarations are visible to importers. If it has no exports, all declarations remain visible for backward compatibility.',
    ],
    syntax: 'import "std/io";\nimport "lib/math";',
    notes: [
      'The `.mettle` extension is added automatically when omitted.',
      'Resolution order: absolute path, `std/` under stdlib, current file directory, then `-I` include directories.',
      'Imports are unconditional; platform-specific code usually lives behind extern declarations or build scripts.',
    ],
  },
  import_str: {
    title: 'import_str',
    body: [
      'Embeds a file as a compile-time `string` value. This is useful for shaders, templates, small assets, test fixtures, and generated text blobs.',
      'Unlike `import`, this reads file contents instead of importing declarations.',
    ],
    syntax: 'var source: string = import_str "shader.glsl";',
    notes: [
      'The path is resolved like a file path, but it is not treated as a Mettle module.',
      'The resulting `string` has `.chars` and `.length`; it is not automatically null-terminated.',
    ],
  },
  export: {
    title: 'export',
    body: [
      'Marks a declaration as part of a module public surface. Once a module exports anything, only exported declarations are visible to importers.',
      'Export applies to declarations defined in the current file.',
    ],
    syntax: 'export fn add(a: int64, b: int64) -> int64 {\n  return a + b;\n}',
    notes: [
      'There is no `private` keyword; omit `export` for implementation details.',
      'Exporting a struct makes its methods visible to importers as part of the struct API.',
    ],
  },
  extern: {
    title: 'extern',
    body: [
      'Declares a symbol implemented outside Mettle, usually by libc, Win32, a linked C file, or an object/library passed to the linker.',
      'The string after `=` is the linker symbol name. Keep signatures ABI-correct; the compiler trusts the declaration.',
    ],
    syntax: 'extern fn puts(msg: cstring) -> int32 = "puts";\nextern var errno_value: int32 = "errno";',
    notes: [
      '`cstring` is the usual type for C `char*`, `void*`, and opaque handles.',
      'For structs passed to C, match C field order, sizes, and padding exactly.',
    ],
  },
  fn: {
    title: 'fn',
    body: [
      'Declares a named function. Parameters and return type are explicit, and arguments are evaluated left to right at call sites.',
      '`fn` is also used for the function pointer type (e.g. `fn(int32, int32) -> int32`): take an address with `&name` and call through the pointer with ordinary call syntax.',
    ],
    syntax: 'fn add(a: int32, b: int32) -> int32 {\n  return a + b;\n}\n\nvar op: fn(int32, int32) -> int32 = &add;\nvar result: int32 = op(3, 4);',
    notes: [
      'Use `-> void` or omit a return value with `return;` for procedures.',
      'Generic functions are monomorphized at compile time: `fn id<T>(x: T) -> T { ... }`.',
      'Two function pointer types are compatible only when parameter types and return type match.',
    ],
  },
  var: {
    title: 'var',
    body: [
      'Declares a variable with an explicit type. Local scalar and pointer variables must be assigned before their first read.',
      'Variables declared in a `for` initializer are scoped to that loop.',
    ],
    syntax: 'var count: int32 = 0;\nvar buf: uint8[256];\nvar ptr: int32* = 0;',
    notes: [
      'Assignment is a statement, not an expression.',
      'String assignment copies the 16-byte string struct, not the underlying character buffer.',
    ],
  },
  struct: {
    title: 'struct',
    body: [
      'Defines a value type with fields laid out in declaration order using target alignment rules.',
      'Structs can define methods, and generic structs are monomorphized at compile time.',
    ],
    syntax: 'struct Point {\n  x: int32;\n  y: int32;\n}',
    notes: [
      'For C interop, match the C layout exactly, including padding fields when necessary.',
      'Arrays and structs are value types; large structs may be passed/returned through ABI-specific paths.',
    ],
  },
  enum: {
    title: 'enum',
    body: [
      'Defines either a plain integer-valued enum or a tagged enum with payload variants.',
      'Plain enum variants are in scope by bare name. Tagged enum variants are constructed with call syntax.',
    ],
    syntax: 'enum Option {\n  Some(int32),\n  None\n}\n\nvar x: Option = Some(42);',
    notes: [
      'Plain enums use `int64` representation.',
      'Tagged enum size depends on the largest payload and discriminant storage.',
      'Use `match` to branch on tagged enum variants and bind payloads.',
    ],
  },
  trait: {
    title: 'trait',
    body: [
      'Declares a compile-time capability that generic code can require. Traits describe required methods or marker constraints.',
      'Trait use is static: generic instantiations are checked and monomorphized by the compiler.',
    ],
    syntax: 'trait Incrementable {\n  fn next_value(self: Self) -> Self;\n}',
    notes: [
      'Use `Self` inside trait method signatures for the implementing type.',
      'Bounds can be inline (`T: Name`) or in a trailing `where` clause.',
    ],
  },
  impl: {
    title: 'impl',
    body: [
      'Satisfies a trait for a concrete type. Methods inside an impl provide the implementation required by the trait.',
      'Impls are used by constrained generic functions during monomorphization.',
    ],
    syntax: 'impl Incrementable for int32 {\n  fn next_value(self: Self) -> Self {\n    return self + 1;\n  }\n}',
    notes: [
      'Generic trait-method calls are statically resolved; this is not dynamic dispatch.',
    ],
  },
  where: {
    title: 'where',
    body: [
      'Adds trailing trait bounds to a generic function or struct. It keeps signatures readable when multiple constraints apply.',
    ],
    syntax: 'fn bump<T>(x: T) -> T where T: Incrementable {\n  return x.next_value();\n}',
    notes: [
      'Multiple bounds use `+`: `where T: Addable + SignedNumber`.',
      'Inline bounds and trailing bounds can be combined.',
    ],
  },
  if: {
    title: 'if',
    body: [
      'Branches on a numeric condition. Zero is false; non-zero is true.',
      'Pointers are not valid directly as conditions; compare them explicitly with `0`.',
    ],
    syntax: 'if (ptr != 0) {\n  use(ptr);\n} else {\n  return 1;\n}',
    notes: [
      '`else if` is supported as normal chained conditionals.',
      'Comparisons produce an `int32` value of 0 or 1.',
    ],
  },
  else: {
    title: 'else',
    body: [
      'Runs when the preceding `if` condition is false. `else if` is parsed as a normal chained conditional.',
    ],
    syntax: 'if (x < 0) {\n  return -1;\n} else if (x == 0) {\n  return 0;\n} else {\n  return 1;\n}',
  },
  while: {
    title: 'while',
    body: [
      'Repeats while a numeric condition is non-zero. Use `while (1)` for an intentional infinite loop.',
    ],
    syntax: 'while (i < len) {\n  total += data[i];\n  i += 1;\n}',
    notes: [
      'A label can appear immediately before a loop: `outer: while (...) { ... }`.',
      'A `defer` inside the loop body runs at the end of each iteration or before `break`/`continue` exits that iteration.',
    ],
  },
  for: {
    title: 'for',
    body: [
      'A C-style loop with initializer, condition, and increment. Any of the three parts may be omitted; `for (;;)` is an infinite loop.',
    ],
    syntax: 'for (var i: int32 = 0; i < n; i += 1) {\n  sum += values[i];\n}',
    notes: [
      'Variables declared in the initializer are scoped to the loop.',
      'The initializer and increment may use assignment or compound assignment statements.',
    ],
  },
  switch: {
    title: 'switch',
    body: [
      'Compares an expression against compile-time constant integer cases. Use `break` to exit a case.',
      'Switch has C-style fall-through when `break` is omitted.',
    ],
    syntax: 'switch (status) {\n  case 0:\n    return 0;\n  default:\n    return 1;\n}',
    notes: [
      'Switch over enum or bool must be exhaustive unless a `default` is present.',
      'Inside a switch nested in a loop, `break` exits the switch; `continue` continues the loop.',
    ],
  },
  case: {
    title: 'case',
    body: [
      'Introduces an arm in `switch` or `match`.',
      'In `match`, `case Variant(binding):` binds a tagged enum payload; in `switch`, `case` uses constant integer expressions.',
    ],
    syntax: 'case Some(value): {\n  return value;\n}',
    notes: [
      '`match` cases do not fall through. `switch` cases can fall through if you omit `break`.',
    ],
  },
  default: {
    title: 'default',
    body: [
      'Fallback arm for `switch` or `match` when no explicit case matches.',
    ],
    syntax: 'default:\n  return 0;',
    notes: [
      'A `match` without `default` must cover every tagged enum variant.',
      'Only one `default` clause is allowed.',
    ],
  },
  match: {
    title: 'match',
    body: [
      'Branches on a tagged enum and can bind payloads from variants.',
      'Match arms do not fall through. Without `default`, all variants must be covered.',
    ],
    syntax: 'match (value) {\n  case Some(v): {\n    return v;\n  }\n  case None: {\n    return 0;\n  }\n}',
    notes: [
      'Payloadless variants use `case Name:` in patterns, but construction currently uses empty call syntax like `None()`.',
      'Expression-form match is supported for value-yielding arms when all arm types unify.',
    ],
  },
  break: {
    title: 'break',
    body: [
      'Exits the innermost loop or switch. With a label, exits the named enclosing loop.',
    ],
    syntax: 'break;\nbreak outer;',
    notes: [
      'A labeled break targets a label attached to an enclosing `while` or `for`.',
      'Deferred statements are emitted before the jump.',
    ],
  },
  continue: {
    title: 'continue',
    body: [
      'Skips to the next iteration of the innermost loop. With a label, continues the named enclosing loop.',
    ],
    syntax: 'continue;\ncontinue outer;',
    notes: [
      'Inside a switch nested in a loop, `continue` continues the loop rather than the switch.',
      'Deferred statements are emitted before the jump.',
    ],
  },
  return: {
    title: 'return',
    body: [
      'Exits the current function. Functions with a non-void return type must return a value.',
    ],
    syntax: 'return;\nreturn value;',
    notes: [
      'For functions with `errdefer`, any non-zero explicit return value is treated as an error path.',
      'Ordinary `defer` statements run on every return path.',
    ],
  },
  defer: {
    title: 'defer',
    body: [
      'Schedules a statement to run when the current scope exits. Deferred statements run in LIFO order: the most recent defer runs first.',
      'The compiler accepts calls, assignments, and blocks after `defer`.',
    ],
    syntax: 'defer fclose(file);\ndefer count = count + 1;\ndefer {\n  flush();\n  close(handle);\n}',
    notes: [
      'Deferred statements capture variables by reference, not by value. In loops, copy values into a temporary if you need the value from that iteration.',
      'Loop-body defers run at the end of each iteration and before `break` or `continue` leaves that iteration.',
    ],
  },
  errdefer: {
    title: 'errdefer',
    body: [
      'Schedules a statement to run only when the function returns a non-zero value. It is convention-based: `0` means success, any non-zero value means error.',
      '`errdefer` is useful for cleanup that should happen on failure but not after successful transfer of ownership.',
    ],
    syntax: 'var buf: cstring = malloc(4096);\nif (buf == 0) {\n  return 1;\n}\nerrdefer free(buf);',
    notes: [
      '`errdefer` is function-only and valid only inside functions.',
      'On an error return, ordinary `defer` and `errdefer` statements both run in the correct LIFO order.',
    ],
  },
  new: {
    title: 'new',
    body: [
      'Allocates a zero-initialized value and returns a pointer to it. The emitted code calls the platform C runtime `calloc(1, size)` directly.',
    ],
    syntax: 'var p: Point* = new Point;',
    notes: [
      'Allocation failure returns null; check `p != 0` before dereferencing if failure is possible.',
      '`new` does not link a Mettle heap runtime by itself.',
    ],
  },
  this: {
    title: 'this',
    body: [
      'Refers to the current method receiver. It is only valid inside method bodies.',
    ],
    syntax: 'method length(this: Vec2*) -> float64 {\n  return sqrt(this->x * this->x + this->y * this->y);\n}',
    notes: [
      'Using `this` as a variable name outside a method is rejected.',
    ],
  },
  asm: {
    title: 'asm',
    body: [
      'Introduces an inline assembly block. This is a low-level escape hatch for instructions or ABI details the language does not expose directly.',
    ],
    syntax: 'asm {\n  mov rax, 60\n}',
    notes: [
      'Keep register clobbers and calling convention expectations explicit in surrounding code.',
      'Inline asm is target-specific; code may not be portable across backends or platforms.',
    ],
  },
  string: {
    title: 'string',
    body: [
      'Built-in 16-byte struct: `.chars` points at bytes and `.length` stores the byte count as `uint64`.',
      'String literals have type `string`. A `string` is distinct from `cstring` and is not automatically null-terminated.',
    ],
    syntax: 'var s: string = "hello";\nvar p: cstring = cstr(s);',
    notes: [
      'Assignment copies the pointer and length, not the pointed-to bytes.',
      'Use `s.chars[i]` for byte indexing and check `i < s.length` yourself.',
    ],
  },
  cstring: {
    title: 'cstring',
    body: [
      'Built-in alias for `uint8*`. Use it for C interop: null-terminated strings, `void*`, `FILE*`, handles, and opaque pointers.',
      '`cstring` and `uint8*` are interchangeable; the name communicates intent at C boundaries.',
    ],
    syntax: 'extern fn puts(msg: cstring) -> int32 = "puts";',
    notes: [
      'Use `cstr(s)` from `std/io` or `s.chars` when a C function expects a pointer.',
      'Pointer arithmetic on `cstring` advances by one byte.',
    ],
  },
  bool: {
    title: 'bool',
    body: [
      'Boolean-like type used by the language and enum exhaustiveness rules. Comparisons produce integer truth values (`0` or `1`).',
    ],
    notes: [
      'Control-flow conditions accept numeric types. Pointers still require explicit comparison, such as `ptr != 0`.',
    ],
  },
  true: {
    title: 'true',
    body: [
      'Boolean true value. In conditions, non-zero values are true.',
    ],
  },
  false: {
    title: 'false',
    body: [
      'Boolean false value. In conditions, zero is false.',
    ],
  },
  Self: {
    title: 'Self',
    body: [
      'Placeholder for the implementing type inside trait method signatures and impl bodies.',
    ],
    syntax: 'trait Incrementable {\n  fn next_value(self: Self) -> Self;\n}',
  },
  sizeof: {
    title: 'sizeof',
    body: [
      'Compile-time size query for a type or value expression. Use it for layout checks, allocation sizes, and ABI assertions.',
    ],
    syntax: 'static_assert(sizeof(int64) == 8);',
    notes: [
      'Struct and tagged enum sizes include target alignment and padding.',
    ],
  },
  alignof: {
    title: 'alignof',
    body: [
      'Compile-time alignment query for a type. Useful when mirroring C layouts or writing low-level allocators.',
    ],
    syntax: 'static_assert(alignof(int64) == 8);',
  },
  static_assert: {
    title: 'static_assert',
    body: [
      'Checks a compile-time condition and fails compilation when it is false.',
      'Use it for ABI layout, size budgets, and assumptions that should never drift silently.',
    ],
    syntax: 'static_assert(sizeof(Header) <= 64);',
  },
}));

for (const [name, detail] of Object.entries(TYPE_SIZES)) {
  if (!HOVER_DOCS.has(name)) {
    HOVER_DOCS.set(name, {
      title: name,
      body: [`Primitive type: ${detail}.`],
      notes: ['Sizes and alignments are for the x86-64 target model documented by Mettle.'],
    });
  }
}

const STDLIB_HOVER_DOCS = {
  cstr: {
    title: 'cstr(s: string) -> cstring',
    body: [
      '`std/io` helper that converts a Mettle `string` into a C-style pointer suitable for APIs expecting `cstring`.',
      'Use this at C interop boundaries such as `puts`, `fopen`, Win32 calls, or helper functions that expect null-terminated input.',
    ],
    syntax: 'import "std/io";\n\nvar msg: string = "hello";\nprintln(cstr(msg));',
    notes: [
      '`string` stores `.chars` and `.length`; `cstring` is just `uint8*`.',
      'Do not assume arbitrary `string` data is safe for C APIs unless it is null-terminated by construction or by the helper.',
    ],
  },
  print: {
    title: 'print(msg: cstring)',
    body: [
      '`std/io` writes a C string to stdout without adding a newline.',
    ],
    syntax: 'print(cstr("status: "));\nprint_int(value);',
    notes: ['Pass `cstring`; use `cstr("text")` for string literals when needed.'],
  },
  println: {
    title: 'println(msg: cstring)',
    body: [
      '`std/io` writes a C string to stdout and appends a newline.',
    ],
    syntax: 'println(cstr("done"));',
    notes: ['For integers, use `println_int(n)` instead of formatting by hand.'],
  },
  print_err: {
    title: 'print_err(msg: cstring)',
    body: [
      '`std/io` writes a C string to stderr without adding a newline.',
    ],
    syntax: 'print_err(cstr("error: "));\nprintln_err(cstr("open failed"));',
  },
  println_err: {
    title: 'println_err(msg: cstring)',
    body: [
      '`std/io` writes a C string to stderr and appends a newline.',
    ],
    syntax: 'println_err(cstr("fatal: missing input"));',
  },
  print_int: {
    title: 'print_int(n: int64)',
    body: [
      '`std/io` writes an integer in decimal without adding a newline.',
    ],
    syntax: 'print(cstr("count="));\nprint_int(count);',
  },
  println_int: {
    title: 'println_int(n: int64)',
    body: [
      '`std/io` writes an integer in decimal and appends a newline.',
    ],
    syntax: 'println_int(total);',
  },
  putchar: {
    title: 'putchar(c: int32) -> int32',
    body: [
      '`std/io` / C runtime function that writes a single byte-like character to stdout.',
    ],
    syntax: "putchar('A');",
  },
  getchar: {
    title: 'getchar() -> int32',
    body: [
      '`std/io` / C runtime function that reads one character from stdin and returns it as `int32`.',
    ],
    notes: ['C `EOF` is represented as a negative value.'],
  },
  fopen: {
    title: 'fopen(filename: cstring, mode: cstring) -> cstring',
    body: [
      '`std/io` wrapper for C `fopen`. Returns an opaque file handle as `cstring`, or `0` on failure.',
    ],
    syntax: 'var fp: cstring = fopen(cstr("data.txt"), cstr("rb"));\nif (fp == 0) {\n  return 1;\n}\ndefer fclose(fp);',
    notes: ['File handles are opaque `FILE*` values represented as `cstring`.'],
  },
  fclose: {
    title: 'fclose(fp: cstring) -> int32',
    body: [
      '`std/io` wrapper for C `fclose`. Closes a file handle returned by `fopen`.',
    ],
    syntax: 'defer fclose(fp);',
    notes: ['Use `defer` immediately after a successful open so all exits close the handle.'],
  },
  fread: {
    title: 'fread(buf: cstring, size: int64, count: int64, fp: cstring) -> int64',
    body: [
      '`std/io` wrapper for C `fread`. Reads up to `count` items of `size` bytes into `buf` and returns the number of items read.',
    ],
    syntax: 'var n: int64 = fread(buf, 1, 4096, fp);',
    notes: ['For byte buffers, pass `size = 1` and `count = capacity`.'],
  },
  fwrite: {
    title: 'fwrite(buf: cstring, size: int64, count: int64, fp: cstring) -> int64',
    body: [
      '`std/io` wrapper for C `fwrite`. Writes up to `count` items of `size` bytes from `buf` and returns the number of items written.',
    ],
    syntax: 'var written: int64 = fwrite(buf, 1, len, fp);',
  },
  fputs: {
    title: 'fputs(s: cstring, fp: cstring) -> int32',
    body: [
      '`std/io` wrapper for C `fputs`. Writes a null-terminated string to a file stream.',
    ],
    syntax: 'fputs(cstr("hello\\n"), fp);',
  },
  fgets: {
    title: 'fgets(buf: cstring, size: int32, fp: cstring) -> cstring',
    body: [
      '`std/io` wrapper for C `fgets`. Reads a line into `buf` and returns `buf`, or `0` on EOF/error.',
    ],
    syntax: 'if (fgets(buf, 256, fp) != 0) {\n  println(buf);\n}',
  },
  fflush: {
    title: 'fflush(fp: cstring) -> int32',
    body: [
      '`std/io` wrapper for C `fflush`. Flushes buffered output for a stream.',
    ],
    syntax: 'fflush(get_stdout());',
  },
  malloc: {
    title: 'malloc(size: int64) -> cstring',
    body: [
      '`std/mem` / C runtime allocation. Allocates `size` bytes and returns a pointer, or `0` on failure.',
    ],
    syntax: 'var buf: cstring = malloc(4096);\nif (buf == 0) {\n  return 1;\n}\ndefer free(buf);',
    notes: [
      'Memory is uninitialized. Use `calloc`, `alloc_zeroed`, or `memset` if you need zeroed bytes.',
      'Use `new T` when allocating a single zero-initialized typed object.',
    ],
  },
  calloc: {
    title: 'calloc(count: int64, size: int64) -> cstring',
    body: [
      '`std/mem` / C runtime allocation. Allocates `count * size` bytes and zero-initializes them.',
    ],
    syntax: 'var items: cstring = calloc(64, sizeof(Item));',
    notes: ['`new T` emits a direct `calloc(1, sizeof(T))` style allocation.'],
  },
  realloc: {
    title: 'realloc(ptr: cstring, size: int64) -> cstring',
    body: [
      '`std/mem` / C runtime resize. Returns a pointer to a block of the new size, or `0` on failure.',
    ],
    notes: ['Keep the old pointer until you know `realloc` succeeded, otherwise failure can lose the allocation.'],
  },
  free: {
    title: 'free(ptr: cstring)',
    body: [
      '`std/mem` / C runtime deallocation. Releases memory allocated by `malloc`, `calloc`, `realloc`, or compatible C allocation paths.',
    ],
    syntax: 'defer free(buf);',
    notes: ['Use `errdefer free(buf)` when ownership transfers away on success but should be cleaned up on failure.'],
  },
  memset: {
    title: 'memset(dest: cstring, value: int32, size: int64) -> cstring',
    body: [
      '`std/mem` / C runtime byte fill.',
    ],
    syntax: 'memset(buf, 0, len);',
  },
  memcpy: {
    title: 'memcpy(dest: cstring, src: cstring, size: int64) -> cstring',
    body: [
      '`std/mem` / C runtime copy for non-overlapping memory regions.',
    ],
    syntax: 'memcpy(dst, src, len);',
    notes: ['If ranges may overlap, use `memmove` instead.'],
  },
  memmove: {
    title: 'memmove(dest: cstring, src: cstring, size: int64) -> cstring',
    body: [
      '`std/mem` / C runtime copy that is safe for overlapping memory regions.',
    ],
    syntax: 'memmove(buf + 1, buf, len - 1);',
  },
  memcmp: {
    title: 'memcmp(a: cstring, b: cstring, size: int64) -> int32',
    body: [
      '`std/mem` / C runtime byte comparison. Returns 0 when the memory ranges are equal.',
    ],
    syntax: 'if (memcmp(a, b, len) == 0) {\n  println(cstr("equal"));\n}',
  },
  strlen: {
    title: 'strlen(s: cstring) -> int64',
    body: [
      '`std/conv` returns the length of a null-terminated C string, not a Mettle `string`.',
    ],
    syntax: 'var len: int64 = strlen(name);',
    notes: ['For Mettle `string`, use the `.length` field instead.'],
  },
  streq: {
    title: 'streq(a: cstring, b: cstring) -> int32',
    body: [
      '`std/conv` helper for null-terminated C string equality. Returns non-zero when equal.',
    ],
    syntax: 'if (streq(ext, cstr(".mettle")) != 0) {\n  return 1;\n}',
  },
  atoi: {
    title: 'atoi(s: cstring) -> int32',
    body: [
      '`std/conv` / C runtime conversion from null-terminated decimal text to `int32`.',
    ],
    notes: ['C `atoi` has weak error reporting; validate input separately when it matters.'],
  },
  atol: {
    title: 'atol(s: cstring) -> int64',
    body: [
      '`std/conv` / C runtime conversion from null-terminated decimal text to `int64`.',
    ],
    notes: ['C `atol` has weak error reporting; validate input separately when it matters.'],
  },
  exit: {
    title: 'exit(code: int32)',
    body: [
      '`std/process` terminates the process immediately with an exit code.',
    ],
    notes: ['Because this leaves the current control path, prefer normal `return` when defers must run.'],
  },
  system: {
    title: 'system(cmd: cstring) -> int32',
    body: [
      '`std/system` runs a shell command through the C runtime and returns its exit code.',
    ],
    syntax: 'var rc: int32 = system(cstr("mettle --version"));',
    notes: ['On Windows this goes through `cmd.exe`; on Linux it goes through `sh -c`.'],
  },
};

for (const [name, doc] of Object.entries(STDLIB_HOVER_DOCS)) {
  HOVER_DOCS.set(name, doc);
}

/**
 * @param {vscode.ExtensionContext} context
 */
function activate(context) {
  diagnosticCollection = vscode.languages.createDiagnosticCollection('mettle');
  context.subscriptions.push(diagnosticCollection);

  mettleOutputChannel = vscode.window.createOutputChannel('Mettle');
  context.subscriptions.push(mettleOutputChannel);

  const lintDocument = async (doc, runCompiler = false) => {
    if (!doc || doc.languageId !== 'mettle') return;
    const uriKey = doc.uri.toString();

    if (!runCompiler && compilingUriKeys.has(uriKey)) {
      return;
    }

    const regexDiags = lintRegex(doc);
    let compilerDiags = [];

    if (runCompiler) {
      compilingUriKeys.add(uriKey);
      try {
        compilerDiags = await lintCompiler(doc);
        compilerDiagnosticsCache.set(uriKey, { version: doc.version, diagnostics: compilerDiags });
      } finally {
        compilingUriKeys.delete(uriKey);
      }
    } else {
      const cached = compilerDiagnosticsCache.get(uriKey);
      compilerDiags = cached && cached.version === doc.version ? cached.diagnostics : [];
    }

    const merged = mergeDiagnostics(regexDiags, compilerDiags);
    diagnosticCollection.set(doc.uri, merged);
  };

  const debouncedLint = (doc) => {
    const uriKey = doc.uri.toString();
    const existing = debounceTimers.get(uriKey);
    if (existing) clearTimeout(existing);
    debounceTimers.set(uriKey, setTimeout(() => {
      debounceTimers.delete(uriKey);
      lintDocument(doc, false);
    }, DEBOUNCE_MS));
  };

  const lintOpenDocuments = async (runCompiler = false) => {
    await Promise.all(
      vscode.workspace.textDocuments
        .filter((doc) => doc.languageId === 'mettle')
        .map((doc) => lintDocument(doc, runCompiler))
    );
  };

  for (const doc of vscode.workspace.textDocuments) {
    if (doc.languageId === 'mettle') lintDocument(doc, true);
  }

  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument((doc) => {
      if (doc.languageId === 'mettle') lintDocument(doc, true);
    }),
    vscode.workspace.onDidCloseTextDocument((doc) => {
      if (doc.languageId !== 'mettle') return;
      const k = doc.uri.toString();
      compilerDiagnosticsCache.delete(k);
      compilingUriKeys.delete(k);
      const timer = debounceTimers.get(k);
      if (timer) clearTimeout(timer);
      debounceTimers.delete(k);
      diagnosticCollection.delete(doc.uri);
    }),
    vscode.workspace.onDidChangeTextDocument((e) => {
      if (e.document.languageId === 'mettle') debouncedLint(e.document);
    }),
    vscode.workspace.onDidSaveTextDocument((doc) => {
      if (doc.languageId === 'mettle') lintDocument(doc, true);
    }),
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (!e.affectsConfiguration('mettle')) return;
      compilerDiagnosticsCache.clear();
      lintOpenDocuments(true);
    }),
    vscode.languages.registerHoverProvider(METTLE_SELECTOR, {
      provideHover(document, position) {
        return provideMettleHover(document, position);
      },
    }),
    vscode.commands.registerCommand('mettle.lintActiveFile', async () => {
      const doc = vscode.window.activeTextEditor?.document;
      if (!doc || doc.languageId !== 'mettle') {
        vscode.window.showInformationMessage('Open a Mettle file to lint it.');
        return;
      }
      await lintDocument(doc, true);
      vscode.window.setStatusBarMessage('Mettle: linted active file', 2500);
    }),
    vscode.commands.registerCommand('mettle.clearDiagnostics', () => {
      compilerDiagnosticsCache.clear();
      diagnosticCollection.clear();
      vscode.window.setStatusBarMessage('Mettle: diagnostics cleared', 2500);
    }),
    vscode.commands.registerCommand('mettle.showOutput', () => {
      mettleOutputChannel?.show();
    })
  );

  // Optimization report panel (+ .explain.txt links, inline loop hints).
  const explain = registerExplain(context, {
    findCompiler,
    log: (line) => mettleOutputChannel?.appendLine(line),
  });
  context.subscriptions.push(
    vscode.commands.registerCommand('mettle.showOptimizationReport', explain.showReport)
  );

  // Codegen annotations: annotated assembly with the compiler decision behind
  // each instruction, in a synced side panel (+ source decorations, CodeLens).
  const codegen = registerCodegen(context, {
    findCompiler,
    log: (line) => mettleOutputChannel?.appendLine(line),
  });
  context.subscriptions.push(
    vscode.commands.registerCommand('mettle.showCodegenAnnotations', codegen.showAnnotations)
  );

  // VTune-style measured codegen profiler: builds + runs the file instrumented
  // with per-basic-block counters, fuses the measured frequency with the static
  // port-cost model, and renders hotspots / port utilization / a flame graph.
  const profile = registerProfile(context, {
    findCompiler,
    log: (line) => mettleOutputChannel?.appendLine(line),
  });
  context.subscriptions.push(
    vscode.commands.registerCommand('mettle.profileCodegen', profile.showProfile)
  );

  // Navigation and editing intelligence (definition, references, rename,
  // symbols, completion, signature help, inlay hints, CodeLens, run/build).
  const lang = registerLanguageFeatures(context, {
    findCompiler,
    log: (line) => mettleOutputChannel?.appendLine(line),
  });
  context.subscriptions.push(
    vscode.commands.registerCommand('mettle.runFile', (uri) => lang.runFile(uri)),
    vscode.commands.registerCommand('mettle.buildFile', (uri) => lang.buildFile(uri))
  );

  // Source-level debugger (F5): compiles with --debug-hooks and drives the
  // instrumented runtime over a named pipe.
  registerDebugAdapter(context, {
    findCompiler,
    log: (line) => mettleOutputChannel?.appendLine(line),
  });
  context.subscriptions.push(
    vscode.commands.registerCommand('mettle.debugFile', (uri) => {
      const program = uri?.fsPath || vscode.window.activeTextEditor?.document?.uri?.fsPath;
      if (!program || !program.endsWith('.mettle')) {
        vscode.window.showInformationMessage('Open a Mettle file to debug it.');
        return;
      }
      vscode.debug.startDebugging(undefined, {
        type: 'mettle',
        request: 'launch',
        name: `Debug ${path.basename(program)}`,
        program,
      });
    })
  );
}

function deactivate() {
  for (const timer of debounceTimers.values()) clearTimeout(timer);
  debounceTimers.clear();
  diagnosticCollection?.dispose();
}

// --- Hover support ---

function provideMettleHover(document, position) {
  const importHover = provideImportPathHover(document, position);
  if (importHover) return importHover;

  const wordRange = document.getWordRangeAtPosition(position, /[A-Za-z_][A-Za-z0-9_]*/);
  if (!wordRange) return undefined;

  const word = document.getText(wordRange);
  const declarationHover = provideDeclarationHover(document, word, position);
  if (declarationHover) return declarationHover;

  const doc = HOVER_DOCS.get(word);
  if (!doc) return undefined;
  return new vscode.Hover(markdownForHoverDoc(doc), wordRange);
}

function provideImportPathHover(document, position) {
  const line = document.lineAt(position.line);
  const text = line.text;
  const match = text.match(/\b(import|import_str)\s+"([^"]*)"/);
  if (!match || match.index === undefined) return undefined;

  const keyword = match[1];
  const rawPath = match[2];
  const pathStart = match.index + match[0].indexOf('"') + 1;
  const pathEnd = pathStart + rawPath.length;
  if (position.character < pathStart || position.character > pathEnd) return undefined;

  const target = resolveImportTarget(document, rawPath, keyword === 'import_str');
  const doc = HOVER_DOCS.get(keyword);
  const md = markdownForHoverDoc(doc);
  md.appendMarkdown('\n\n---\n\n');
  md.appendMarkdown(`**Path:** \`${rawPath || '(empty)'}\`\n\n`);
  if (target) {
    md.appendMarkdown(`**Resolved:** \`${target}\`\n\n`);
  } else {
    md.appendMarkdown('**Resolved:** not found with the current workspace/settings search path.\n\n');
  }
  md.appendMarkdown(keyword === 'import'
    ? 'The compiler appends `.mettle` when the import path has no extension.'
    : '`import_str` embeds bytes as a `string`; it does not import declarations.');

  return new vscode.Hover(md, new vscode.Range(position.line, pathStart, position.line, pathEnd));
}

function provideDeclarationHover(document, word, position) {
  const text = document.getText();
  const masked = maskNonCode(text);
  const originalLines = text.split(/\r?\n/);
  const maskedLines = masked.split(/\r?\n/);
  const declarations = findDeclarations(document, originalLines, maskedLines);

  const declaration = declarations.find((d) => d.name === word);
  if (!declaration) return undefined;

  const wordOffset = document.offsetAt(position);
  if (wordOffset >= declaration.nameStartOffset && wordOffset <= declaration.nameEndOffset) {
    // Prefer the richer declaration card even when hovering the declaration itself.
  }

  const md = new vscode.MarkdownString(undefined, true);
  md.isTrusted = false;
  md.appendMarkdown(`**${declaration.kind}: ${escapeMarkdown(declaration.name)}**\n\n`);
  appendFence(md, declaration.signature, 'mettle');

  if (declaration.doc.length > 0) {
    md.appendMarkdown(`\n${declaration.doc.map(escapeMarkdown).join('\n')}\n`);
  }

  if (declaration.detail) {
    md.appendMarkdown(`\n${declaration.detail}\n`);
  }

  return new vscode.Hover(md);
}

function markdownForHoverDoc(doc) {
  const md = new vscode.MarkdownString(undefined, true);
  md.isTrusted = false;
  md.appendMarkdown(`**${doc.title}**\n\n`);
  for (const paragraph of doc.body || []) {
    md.appendMarkdown(`${paragraph}\n\n`);
  }
  if (doc.syntax) {
    appendFence(md, doc.syntax, 'mettle');
  }
  if (doc.notes && doc.notes.length > 0) {
    md.appendMarkdown('\n**Notes**\n\n');
    for (const note of doc.notes) {
      md.appendMarkdown(`- ${note}\n`);
    }
  }
  return md;
}

function appendFence(md, code, language) {
  md.appendMarkdown(`\n\`\`\`${language}\n${code}\n\`\`\`\n`);
}

function escapeMarkdown(value) {
  return String(value).replace(/[\\`*_{}[\]()#+\-.!|>]/g, '\\$&');
}

function maskNonCode(text) {
  let out = '';
  let i = 0;
  let blockDepth = 0;
  let inString = false;
  let inChar = false;
  let inLineComment = false;

  while (i < text.length) {
    const ch = text[i];
    const next = text[i + 1];

    if (ch === '\r' || ch === '\n') {
      out += ch;
      i++;
      inLineComment = false;
      continue;
    }

    if (inLineComment) {
      out += ' ';
      i++;
      continue;
    }

    if (inString) {
      if (ch === '\\') {
        out += ' ';
        if (next) out += next === '\n' || next === '\r' ? next : ' ';
        i += 2;
        continue;
      }
      out += ' ';
      if (ch === '"') inString = false;
      i++;
      continue;
    }

    if (inChar) {
      if (ch === '\\') {
        out += ' ';
        if (next) out += next === '\n' || next === '\r' ? next : ' ';
        i += 2;
        continue;
      }
      out += ' ';
      if (ch === "'") inChar = false;
      i++;
      continue;
    }

    if (blockDepth > 0) {
      if (ch === '/' && next === '*') {
        out += '  ';
        i += 2;
        blockDepth++;
        continue;
      }
      if (ch === '*' && next === '/') {
        out += '  ';
        i += 2;
        blockDepth--;
        continue;
      }
      out += ' ';
      i++;
      continue;
    }

    if (ch === '/' && next === '/') {
      out += '  ';
      i += 2;
      inLineComment = true;
      continue;
    }
    if (ch === '/' && next === '*') {
      out += '  ';
      i += 2;
      blockDepth = 1;
      continue;
    }
    if (ch === '"') {
      out += ' ';
      i++;
      inString = true;
      continue;
    }
    if (ch === "'") {
      out += ' ';
      i++;
      inChar = true;
      continue;
    }

    out += ch;
    i++;
  }

  return out;
}

function findDeclarations(document, originalLines, maskedLines) {
  const declarations = [];
  const patterns = [
    {
      kind: 'function',
      re: /^\s*(?:export\s+)?(?:extern\s+)?function\s+([A-Za-z_][A-Za-z0-9_]*)\b/,
      detail: (line) => line.includes('extern') ? 'Extern function declaration. The optional string literal names the linker symbol.' : 'Function declaration.',
    },
    {
      kind: 'function pointer type',
      re: /^\s*(?:export\s+)?(?:extern\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)\b/,
      detail: () => 'Function pointer related declaration.',
    },
    {
      kind: 'method',
      re: /^\s*(?:export\s+)?method\s+([A-Za-z_][A-Za-z0-9_]*)\b/,
      detail: () => 'Method declaration. `this` refers to the receiver inside the body.',
    },
    {
      kind: 'struct',
      re: /^\s*(?:export\s+)?struct\s+([A-Za-z_][A-Za-z0-9_]*)\b/,
      detail: (line, startLine) => declarationBlockDetail(originalLines, startLine, 'field'),
    },
    {
      kind: 'enum',
      re: /^\s*(?:export\s+)?enum\s+([A-Za-z_][A-Za-z0-9_]*)\b/,
      detail: (line, startLine) => declarationBlockDetail(originalLines, startLine, 'variant'),
    },
    {
      kind: 'trait',
      re: /^\s*(?:export\s+)?trait\s+([A-Za-z_][A-Za-z0-9_]*)\b/,
      detail: (line, startLine) => declarationBlockDetail(originalLines, startLine, 'requirement'),
    },
    {
      kind: 'global variable',
      re: /^\s*(?:export\s+)?(?:extern\s+)?var\s+([A-Za-z_][A-Za-z0-9_]*)\b/,
      detail: (line) => line.includes('extern') ? 'Extern variable declaration. The optional string literal names the linker symbol.' : 'Global variable declaration.',
    },
  ];

  for (let lineNo = 0; lineNo < maskedLines.length; lineNo++) {
    const maskedLine = maskedLines[lineNo];
    for (const pattern of patterns) {
      const match = maskedLine.match(pattern.re);
      if (!match || match.index === undefined) continue;

      const name = match[1];
      const nameCol = maskedLine.indexOf(name, match.index);
      const nameStart = document.offsetAt(new vscode.Position(lineNo, nameCol));
      declarations.push({
        name,
        kind: pattern.kind,
        signature: collectSignature(originalLines, lineNo),
        doc: collectLeadingDoc(originalLines, lineNo),
        detail: pattern.detail(maskedLine, lineNo),
        nameStartOffset: nameStart,
        nameEndOffset: nameStart + name.length,
      });
    }

    const impl = maskedLine.match(/^\s*impl\s+(.+?)\s+for\s+([A-Za-z_][A-Za-z0-9_:.<>]*)\b/);
    if (impl) {
      const label = `${impl[1].trim()} for ${impl[2].trim()}`;
      const implCol = maskedLine.indexOf(impl[2]);
      const nameStart = document.offsetAt(new vscode.Position(lineNo, implCol));
      declarations.push({
        name: impl[2].trim().split(/[.:<]/)[0],
        kind: 'impl',
        signature: collectSignature(originalLines, lineNo),
        doc: collectLeadingDoc(originalLines, lineNo),
        detail: `Implements \`${label}\`.`,
        nameStartOffset: nameStart,
        nameEndOffset: nameStart + impl[2].trim().length,
      });
    }
  }

  return declarations;
}

function collectSignature(lines, startLine) {
  const parts = [];
  let depth = 0;
  for (let i = startLine; i < Math.min(lines.length, startLine + 12); i++) {
    const line = lines[i].trimEnd();
    parts.push(line);
    for (const ch of line) {
      if (ch === '(' || ch === '<') depth++;
      if (ch === ')' || ch === '>') depth = Math.max(0, depth - 1);
    }
    if (depth === 0 && /[{;]\s*(?:\/\/.*)?$/.test(line)) break;
  }
  return parts.join('\n').trim();
}

function collectLeadingDoc(lines, startLine) {
  const docs = [];
  for (let i = startLine - 1; i >= 0; i--) {
    const trimmed = lines[i].trim();
    if (trimmed === '') {
      if (docs.length === 0) continue;
      break;
    }
    const doc = trimmed.match(/^\/\/\/\s?(.*)$/);
    if (!doc) break;
    docs.unshift(doc[1]);
  }
  return docs;
}

function declarationBlockDetail(lines, startLine, noun) {
  let opened = false;
  let depth = 0;
  let count = 0;
  for (let i = startLine; i < Math.min(lines.length, startLine + 300); i++) {
    const line = lines[i];
    for (const ch of line) {
      if (ch === '{') {
        opened = true;
        depth++;
      } else if (ch === '}') {
        depth--;
      }
    }
    if (opened && depth > 0 && i > startLine && /\b[A-Za-z_][A-Za-z0-9_]*\b/.test(line) && !/^\s*(\/\/|\/\*)/.test(line)) {
      count++;
    }
    if (opened && depth <= 0) break;
  }
  if (!opened || count === 0) return '';
  return `${count} ${noun}${count === 1 ? '' : 's'} declared in this block.`;
}

function resolveImportTarget(document, rawPath, isTextImport) {
  if (!rawPath) return null;
  const filePath = document.uri.fsPath;
  const workspaceRoot = vscode.workspace.getWorkspaceFolder(document.uri)?.uri?.fsPath || path.dirname(filePath);
  const cfg = getConfig();
  const candidates = [];
  const add = (base, importPath, appendMettle) => {
    if (!base && !path.isAbsolute(importPath)) return;
    const resolved = path.isAbsolute(importPath) ? importPath : path.join(base, importPath);
    candidates.push(resolved);
    if (appendMettle && path.extname(resolved) === '') candidates.push(`${resolved}.mettle`);
  };

  const appendMettle = !isTextImport;
  if (path.isAbsolute(rawPath)) add('', rawPath, appendMettle);

  if (rawPath.startsWith('std/') || rawPath.startsWith('std\\')) {
    const stdlib = resolveStdlibRoot(workspaceRoot, filePath, cfg);
    if (stdlib) add(stdlib, rawPath, appendMettle);
  }

  add(path.dirname(filePath), rawPath, appendMettle);
  add(workspaceRoot, rawPath, appendMettle);

  for (const includePath of cfg.extraIncludePaths || []) {
    if (!includePath || typeof includePath !== 'string') continue;
    add(path.isAbsolute(includePath) ? includePath : path.join(workspaceRoot, includePath), rawPath, appendMettle);
  }

  for (const candidate of candidates) {
    if (fs.existsSync(candidate) && fs.statSync(candidate).isFile()) {
      return candidate;
    }
  }
  return null;
}

function resolveStdlibRoot(workspaceRoot, filePath, cfg) {
  if (cfg.stdlibPath) {
    const p = path.isAbsolute(cfg.stdlibPath) ? cfg.stdlibPath : path.join(workspaceRoot, cfg.stdlibPath);
    if (fs.existsSync(p)) return p;
  }

  let dir = path.resolve(path.dirname(filePath));
  for (let depth = 0; depth < 16; depth++) {
    const candidate = path.join(dir, 'stdlib');
    if (fs.existsSync(candidate)) return candidate;
    const parent = path.dirname(dir);
    if (parent === dir) break;
    dir = parent;
  }

  const workspaceCandidate = path.join(workspaceRoot, 'stdlib');
  if (fs.existsSync(workspaceCandidate)) return workspaceCandidate;
  return null;
}

/**
 * Merge regex and compiler diagnostics. Compiler takes precedence on overlapping ranges.
 */
function mergeDiagnostics(regex, compiler) {
  const byKey = new Map();
  for (const d of regex) {
    const k = `${d.range.start.line}:${d.range.start.character}`;
    byKey.set(k, d);
  }
  for (const d of compiler) {
    const k = `${d.range.start.line}:${d.range.start.character}`;
    byKey.set(k, d); // compiler overwrites regex for same position
  }
  return [...byKey.values()];
}

// --- Regex-based linting ---

function getSegments(line, lineIdx, state) {
  const segments = [];
  let i = 0;
  let segmentStart = 0;

  while (i < line.length) {
    if (state.inString) {
      if (line[i] === '\\') { i += 2; continue; }
      if (line[i] === '"') { state.inString = false; i++; segmentStart = i; continue; }
      i++;
      continue;
    }
    if (state.blockDepth > 0) {
      if (line.indexOf('/*', i) === i) {
        state.blockDepth++;
        i += 2;
        segmentStart = i;
        continue;
      }
      if (line.indexOf('*/', i) === i) {
        state.blockDepth--;
        i += 2;
        segmentStart = i;
        continue;
      }
      i++;
      continue;
    }
    if (line.indexOf('//', i) === i) {
      if (i > segmentStart) segments.push({ start: segmentStart, end: i, inString: false });
      break;
    }
    if (line.indexOf('/*', i) === i) {
      if (i > segmentStart) segments.push({ start: segmentStart, end: i, inString: false });
      state.blockDepth = 1;
      state.blockStartLine = lineIdx;
      state.blockStartCol = i;
      i += 2;
      segmentStart = i;
      continue;
    }
    if (line[i] === '"') {
      if (i > segmentStart) segments.push({ start: segmentStart, end: i, inString: false });
      state.inString = true;
      state.stringStartLine = lineIdx;
      state.stringStartCol = i;
      i++;
      segmentStart = i;
      continue;
    }
    i++;
  }
  if (segmentStart < line.length && !state.inString && state.blockDepth === 0) {
    segments.push({ start: segmentStart, end: line.length, inString: false });
  }
  return segments;
}

function lintRegex(document) {
  const diagnostics = [];
  const text = document.getText();
  const lines = text.split(/\r?\n/);
  const state = {
    inString: false,
    stringStartLine: 0,
    stringStartCol: 0,
    blockDepth: 0,
    blockStartLine: 0,
    blockStartCol: 0,
  };

  for (let lineIdx = 0; lineIdx < lines.length; lineIdx++) {
    const line = lines[lineIdx];
    const segments = getSegments(line, lineIdx, state);

    for (const seg of segments) {
      const slice = line.slice(seg.start, seg.end);
      const add = (re, msg) => {
        let m;
        const r = new RegExp(re.source, 'g');
        while ((m = r.exec(slice)) !== null) {
          const col = seg.start + m.index;
          diagnostics.push(mkDiag(lineIdx, col, m[0].length, msg, 'Mettle'));
        }
      };

      add(/\b0[xX](?=\s|$|[^0-9a-fA-F])/g, 'Invalid hex literal. Expected hex digits after 0x.');
      add(/\b0[bB](?=\s|$|[^01])/g, 'Invalid binary literal. Expected 0 or 1 after 0b.');
      add(/\b[0-9]+\d*_[0-9_]*\b/g, 'Underscores in numeric literals are not supported. Use 1000000 instead of 1_000_000.');
    }
  }

  if (state.inString) {
    diagnostics.push(mkDiag(state.stringStartLine, state.stringStartCol, 1, 'Unterminated string literal. Add closing double quote.', 'Mettle'));
  }
  if (state.blockDepth > 0) {
    diagnostics.push(mkDiag(state.blockStartLine, state.blockStartCol, 2, 'Unterminated block comment. Add closing */.', 'Mettle'));
  }

  return diagnostics;
}

function mkDiag(line, col, len, msg, source = 'Mettle') {
  const d = new vscode.Diagnostic(
    new vscode.Range(line, col, line, col + len),
    msg,
    vscode.DiagnosticSeverity.Error
  );
  d.source = source;
  return d;
}

// --- Compiler-backed linting ---

function getConfig() {
  const cfg = vscode.workspace.getConfiguration('mettle');
  return {
    compilerEnabled: cfg.get('linter.compilerEnabled', true),
    compilerPath: cfg.get('linter.compilerPath', null),
    stdlibPath: cfg.get('linter.stdlibPath', null),
    extraIncludePaths: cfg.get('linter.extraIncludePaths', []),
    compilerTimeoutMs: cfg.get('linter.compilerTimeoutMs', 10000),
  };
}

/**
 * Walk up from `startDir` looking for bin/mettle(.exe) so a workspace opened on a
 * subfolder (e.g. only `mettle/`) still finds the repo compiler in `../bin/`.
 * @param {string} startDir
 * @returns {string | null}
 */
function findCompilerInAncestors(startDir) {
  let dir = path.resolve(startDir);
  for (let depth = 0; depth < 16; depth++) {
    const win = path.join(dir, 'bin', 'mettle.exe');
    const unix = path.join(dir, 'bin', 'mettle');
    if (fs.existsSync(win)) return win;
    if (fs.existsSync(unix)) return unix;
    const parent = path.dirname(dir);
    if (parent === dir) break;
    dir = parent;
  }
  return null;
}

/**
 * @param {string} workspaceRoot
 * @param {string} filePath - current .mettle file path (used for ancestor search)
 */
function findCompiler(workspaceRoot, filePath) {
  const cfg = getConfig();
  if (cfg.compilerPath) {
    const p = path.isAbsolute(cfg.compilerPath) ? cfg.compilerPath : path.join(workspaceRoot, cfg.compilerPath);
    if (fs.existsSync(p)) return p;
  }
  const fromAncestors = findCompilerInAncestors(path.dirname(filePath));
  if (fromAncestors) return fromAncestors;
  const candidates = [
    path.join(workspaceRoot, 'bin', 'mettle.exe'),
    path.join(workspaceRoot, 'bin', 'mettle'),
    path.join(workspaceRoot, 'mettle.exe'),
    path.join(workspaceRoot, 'mettle'),
  ];
  for (const c of candidates) {
    if (fs.existsSync(c)) return c;
  }
  return process.platform === 'win32' ? 'mettle.exe' : 'mettle';
}

/**
 * Run Mettle compiler and parse stderr/stdout for errors.
 * Compiler prints either:
 *   error: msg\n  --> path:line:column  (when filename is known), or
 *   error: msg\n  --> line N, column M  (when filename is absent)
 */
async function lintCompiler(document) {
  const cfg = getConfig();
  if (!cfg.compilerEnabled) return [];

  const filePath = document.uri.fsPath;
  if (!filePath || !fs.existsSync(filePath)) return [];

  const workspaceRoot = vscode.workspace.getWorkspaceFolder(document.uri)?.uri?.fsPath || path.dirname(filePath);
  const compiler = findCompiler(workspaceRoot, filePath);
  if (!compiler) return [];

  let tempDir;
  try {
    tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'mettle-lint-'));
  } catch (err) {
    mettleOutputChannel?.appendLine(`[Mettle] Could not create lint temp directory: ${err.message}`);
    return [];
  }
  const tempOut = path.join(tempDir, 'lint.s');

  const args = [
    '-i', filePath,
    '-o', tempOut,
    '-I', path.dirname(filePath),
    '-I', workspaceRoot,
  ];
  if (cfg.stdlibPath) {
    const stdlib = path.isAbsolute(cfg.stdlibPath)
      ? cfg.stdlibPath
      : path.join(workspaceRoot, cfg.stdlibPath);
    args.push('--stdlib', stdlib);
  }
  for (const includePath of cfg.extraIncludePaths || []) {
    if (!includePath || typeof includePath !== 'string') continue;
    args.push('-I', path.isAbsolute(includePath) ? includePath : path.join(workspaceRoot, includePath));
  }

  return new Promise((resolve) => {
    execFile(compiler, args, {
      timeout: Math.max(1000, Number(cfg.compilerTimeoutMs) || 10000),
      maxBuffer: 1024 * 1024,
      cwd: workspaceRoot,
    }, (err, stdout, stderr) => {
      try { fs.rmSync(tempDir, { recursive: true, force: true }); } catch (_) {}

      const output = (stdout || '') + (stderr || '');
      const diagnostics = parseCompilerOutput(output, document, workspaceRoot);

      if (diagnostics.length === 0 && err && /** @type {NodeJS.ErrnoException} */ (err).code === 'ENOENT') {
        mettleOutputChannel?.appendLine(
          `[Mettle] Compiler not found: ${compiler}\n` +
            `Set mettle.linter.compilerPath to your mettle executable, or add it to PATH.\n` +
            `Tried repo bin/ by walking up from: ${path.dirname(filePath)}`
        );
        mettleOutputChannel?.show(true);
      } else if (diagnostics.length === 0 && output.trim().length > 0 && /error:/i.test(output)) {
        mettleOutputChannel?.appendLine(
          `[Mettle] Could not parse compiler output for ${filePath}. First lines:\n${output.slice(0, 800)}`
        );
      }

      resolve(diagnostics);
    });
  });
}

/**
 * Strip ANSI codes and parse compiler diagnostics.
 * @param {string} workspaceRoot - used to resolve relative paths in `path:line:column` locations
 */
function parseCompilerOutput(output, document, workspaceRoot) {
  const diagnostics = [];
  const stripped = output.replace(/\x1b\[[0-9;]*m/g, '');
  const lines = stripped.split(/\r?\n/);

  const docPath = path.normalize(document.uri.fsPath);

  /** @param {string} p */
  function resolveReportedPath(p) {
    const trimmed = p.trim();
    if (path.isAbsolute(trimmed)) return path.normalize(trimmed);
    return path.normalize(path.resolve(workspaceRoot, trimmed));
  }

  for (let i = 0; i < lines.length - 1; i++) {
    const header = lines[i].match(/^(error|warning|note):\s*(.+)$/);
    if (!header) continue;

    const kind = header[1];
    const msg = header[2].trim();
    const locText = lines[i + 1];
    if (!locText || !locText.includes('-->')) continue;

    let line1Based;
    let col1Based;

    const lineColForm = locText.match(/^\s*-->\s*line\s+(\d+),\s*column\s+(\d+)/);
    const pathForm = locText.match(/^\s*-->\s*(.+):(\d+):(\d+)\s*$/);

    if (lineColForm) {
      line1Based = parseInt(lineColForm[1], 10);
      col1Based = parseInt(lineColForm[2], 10);
    } else if (pathForm) {
      const reportedPath = resolveReportedPath(pathForm[1]);
      if (!pathsEqualish(reportedPath, docPath)) continue;
      line1Based = parseInt(pathForm[2], 10);
      col1Based = parseInt(pathForm[3], 10);
    } else {
      continue;
    }

    const severity =
      kind === 'warning'
        ? vscode.DiagnosticSeverity.Warning
        : kind === 'note'
          ? vscode.DiagnosticSeverity.Information
          : vscode.DiagnosticSeverity.Error;

    const line = Math.max(0, line1Based - 1);
    const col = Math.max(0, col1Based - 1);

    const safeLine = document.lineCount > 0 ? Math.min(line, document.lineCount - 1) : 0;
    const lineText = document.lineCount > 0 ? document.lineAt(safeLine).text : '';
    const endCol = Math.min(col + 1, Math.max(lineText.length, 1));

    const d = new vscode.Diagnostic(
      new vscode.Range(safeLine, col, safeLine, endCol),
      msg,
      severity
    );
    d.source = 'Mettle';
    diagnostics.push(d);
  }

  return diagnostics;
}

/** Case-insensitive normalized path compare (Windows-friendly; resolves symlinks when possible). */
function pathsEqualish(a, b) {
  const na = path.normalize(a).toLowerCase();
  const nb = path.normalize(b).toLowerCase();
  if (na === nb) return true;
  try {
    const ra = fs.realpathSync(a);
    const rb = fs.realpathSync(b);
    return path.normalize(ra).toLowerCase() === path.normalize(rb).toLowerCase();
  } catch {
    return false;
  }
}

module.exports = { activate, deactivate };
