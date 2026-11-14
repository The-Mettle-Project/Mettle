#include "error_explain.h"
#include <stdio.h>
#include <stdlib.h>
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
     "literal such as 0x with no digits, or a stray character that is not\n"
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
     "boundary and keeps going, so one mistake reports once.\n"},
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
     "A written type does not bend to fit its value, so both sides must line\n"
     "up.\n"
     "\n"
     "Example:\n"
     "    var x: int64 = \"hello\";   // expected 'int64', found 'string'\n"
     "\n"
     "Fixes:\n"
     "  - change the declared type to match the value, or the value to\n"
     "    match the type\n"
     "  - for numeric conversions, cast explicitly: (int32)value\n"
     "\n"
     "Integers are the exception, and only in one direction: a conversion\n"
     "that every value survives happens on its own (int32 -> int64), and one\n"
     "that can change the value needs the cast. Those report M0119 rather\n"
     "than this code, so `mettle explain M0119` has the rule.\n"
     "\n"
     "`Type` and `Field` are compile-time reflection values (a TypeRef is\n"
     "a type-table index; a FieldRef is {type_index, field_index}). They\n"
     "have no runtime representation and cannot escape into runtime code.\n"
     "Bind them with `const` and keep them at compile time.\n"},
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
     "A memcpy or memset writes more bytes than the\n"
     "destination stack array holds, smashing adjacent stack memory.\n"
     "\n"
     "Fix: pass the correct byte count (element count * element size), or\n"
     "grow the buffer.\n"},
    {"M0107", "Memory leak",
     "An allocation is never returned, stored or passed on, and never freed,\n"
     "so the memory is unreachable after the function returns.\n"
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
     "A pointer into a stack value, a field or an array element, escapes the\n"
     "scope that owns the value, for example saved to an outer variable\n"
     "inside a block. When the block exits the pointee dies.\n"
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
    {"M0113", "Dereference of a null pointer",
     "The pointer was assigned null and never reassigned before this use,\n"
     "so the dereference traps the moment it runs.\n"
     "\n"
     "Fix: assign a real address first, or guard the access:\n"
     "    if (p != 0) { ... }\n"
     "\n"
     "Only reported when the null assignment and the use are provably on\n"
     "the same path, so this is a fact rather than a suspicion.\n"},
    {"M0114", "Dereference of an unmapped constant address",
     "The pointer holds a small integer constant. The low 64K of the\n"
     "address space is never mapped on any supported platform, so this\n"
     "dereference faults.\n"
     "\n"
     "Usually a placeholder that was never filled in, or an integer that\n"
     "was cast to a pointer by mistake.\n"},
    {"M0115", "Shift count at or past the operand width",
     "Shifting a 32-bit value by 32, or a 64-bit value by 64, does not\n"
     "produce zero. The hardware masks the count (x86 uses the low 5 or 6\n"
     "bits), so `x << 32` on an int32 is `x << 0`, which is `x`.\n"
     "\n"
     "Fix: shift by less than the width, or widen the operand first:\n"
     "    var wide: int64 = (int64)x;\n"
     "    var shifted: int64 = wide << 32;\n"},
    {"M0116", "Division or modulo by a constant zero",
     "The divisor is the literal zero, so the operation traps the moment it\n"
     "executes. This is a hard error: no input can make it work.\n"
     "\n"
     "Fix: correct the constant, or guard the divisor when it is meant to\n"
     "be a variable.\n"},
    {"M0117", "Loop index runs past the end of the array",
     "The loop's upper bound and the array's length are both known at\n"
     "compile time, and the bound is the larger. The final iteration reads\n"
     "or writes past the end.\n"
     "\n"
     "Example:\n"
     "    var a: int64[8];\n"
     "    for i in 0..9 { a[i] = i; }   // M0117: valid indexes are 0..7\n"
     "\n"
     "Fix: match the bound to the length. Indexes are 0-based, so an array\n"
     "of 8 runs 0..7 and an off-by-one here is the usual cause.\n"},
    {"M0118", "Integer out of range for its destination",
     "A compile-time integer is stored somewhere that cannot hold it. The\n"
     "value is known here, so this is a fact rather than a risk: the store\n"
     "would keep the low bits and discard the rest.\n"
     "\n"
     "Example:\n"
     "    var h: int32 = 2654435761;    // M0118: int32 holds ..2147483647\n"
     "\n"
     "Fixes:\n"
     "  - widen the destination: `var h: int64 = 2654435761;`\n"
     "  - pick the type that holds it: `var h: uint32 = 2654435761;`\n"
     "  - cast, when the wrap is the point: `(int32)2654435761`\n"
     "\n"
     "The last one is not a workaround. A cast at the site is how a reader\n"
     "learns the truncation was intended, which is the whole reason this is\n"
     "reported instead of performed.\n"},
     {"M0119", "Narrowing conversion needs a cast",
     "A value flows into a type that cannot hold every value the source type\n"
     "can. Mettle converts silently in one direction only:\n"
     "\n"
     "    Widen silently. Narrow loudly.\n"
     "\n"
     "So int32 -> int64 and uint32 -> int64 need nothing written, because\n"
     "every value survives them. int64 -> int32, uint64 -> int64 and\n"
     "int32 -> uint32 do, because some values do not.\n"
     "\n"
     "Example:\n"
     "    fn f(n: int64) {\n"
     "        var small: int32 = n;         // M0119\n"
     "        var ok:    int32 = (int32)n;  // says the wrap is meant\n"
     "    }\n"
     "\n"
     "A compile-time constant is exempt: its value is known, so\n"
     "`var b: uint8 = 200;` is checked rather than refused (see M0118 for\n"
     "the case where it does not fit).\n"
     "\n"
     "Two destinations sit outside the rule because they are not range\n"
     "conversions: `bool` is a truth coercion, and an enum names a set.\n"
     "float32 -> float64 widens silently and float64 -> float32 stays silent.\n"
     "float32 -> float16 and float32 -> bfloat16 narrow loudly, like int64 ->\n"
     "int8. A float literal that is exactly representable needs no cast.\n"},
    {"M0120", "Pointer cast to an integer and back to a pointer",
     "A pointer is cast to an integer and straight back to a pointer. The\n"
     "integer in the middle holds the same address the pointer already held,\n"
     "so nothing about the value changes. What changes is what the compiler\n"
     "can say about it: an integer has no provenance, so the borrow checker,\n"
     "the alias analysis and --verify all have to give up on where that\n"
     "address came from.\n"
     "\n"
     "Example:\n"
     "    var p: float32* = (float32*)((int64)(&(buf[0])));  // M0120\n"
     "    var q: float32* = &(buf[0]);                       // the same value\n"
     "\n"
     "Lowering sees through the round trip, so the analyses are not blinded\n"
     "while the spelling is cleaned up. The value is not wrong; the detour\n"
     "is.\n"
     "\n"
     "Neither half is reported on its own, because each is sometimes the only\n"
     "way to say a thing. An integer that is an address, such as a handle\n"
     "from the operating system or a device pointer, becomes a pointer by\n"
     "cast. A pointer becomes an integer to be printed, hashed or aligned.\n"
     "Only the round trip carries no information.\n"},
    {"M0121", "A task was handed a pointer into the frame that spawned it",
     "A call hands the address of a function this program defines to a callee\n"
     "whose body is not here, with a pointer straight after it. That is what\n"
     "every thread-spawn interface looks like, so the pointer is what the new\n"
     "task will read. The pointer given points into the spawning frame, and\n"
     "the frame's storage is gone the moment it returns.\n"
     "\n"
     "Example:\n"
     "    var arg: Work;\n"
     "    CreateThread(0, 0, &worker, &arg, 0, 0);   // M0121\n"
     "\n"
     "The compiler cannot see when the task stops, so it cannot prove the\n"
     "frame outlives it. This is refused rather than warned about, because\n"
     "the read happens on another thread and the corruption it causes has no\n"
     "line number on it.\n"
     "\n"
     "Fixes:\n"
     "  - hand over an allocation, which the task or a later join frees\n"
     "  - hand over the address of a global, which outlives every frame\n"
     "  - pass the value by copy where the interface allows it\n"},
    {"M0122", "A message was written after it was handed to a task",
     "A pointer was given to a task and then written through here. Both ends\n"
     "hold the same object from that point on, with nothing ordering the two\n"
     "writes, so what the task reads depends on which core got there first.\n"
     "\n"
     "Example:\n"
     "    CreateThread(0, 0, &worker, msg, 0, 0);\n"
     "    msg.count = 7;                             // M0122\n"
     "\n"
     "Fixes:\n"
     "  - finish writing the message before the task starts\n"
     "  - hand the task its own copy and keep writing this one\n"
     "  - take the pointer back out of a structure the two ends share under a\n"
     "    lock, so the write is ordered by something the reader also takes\n"},
    {"M0123", "A signed operation left its type",
     "Signed `+`, `-` and `*` in Mettle wrap rather than trapping, the way the\n"
     "machine does. `--check-overflow` says to stop instead, and this is what\n"
     "it stops on: the result of the operation is not a value the type can\n"
     "hold.\n"
     "\n"
     "Example:\n"
     "    var big: int32 = 2000000000;\n"
     "    var sum: int32 = big + big;    // M0123 under --check-overflow\n"
     "\n"
     "The check is not emitted where a declared type already bounds the\n"
     "operands tightly enough that the result has to fit:\n"
     "\n"
     "    type Half = int32 where value >= 0 && value <= 1000000000;\n"
     "    fn blend(a: Half, b: Half) -> int32 { return a + b; }\n"
     "\n"
     "Two Halves sum to at most two billion, which an int32 holds, so nothing\n"
     "is emitted and `--explain` reports the deletion under **proven by\n"
     "type**. A program whose every signed operation is proven that way\n"
     "compiles to the same bytes with the flag as without it.\n"
     "\n"
     "Unsigned arithmetic is not checked. Wrapping is what unsigned types are\n"
     "reached for in a hash or a checksum, and a check there would refuse the\n"
     "programs that want it.\n"},
    {"M0125", "A signed value compared with an unsigned one",
     "A comparison of a signed and an unsigned integer happens in the wider\n"
     "of the two types. When the signed side is the wider, every value of both\n"
     "fits and the answer is exact. When the unsigned side is as wide or wider,\n"
     "the comparison is unsigned, so a negative value reads as a huge one.\n"
     "\n"
     "Example:\n"
     "    var i: int32 = -1;\n"
     "    if (i < s.length) { ... }     // M0125: false, s.length is uint64\n"
     "\n"
     "The warning stays quiet when the signed side cannot be negative: a\n"
     "constant, a `for` range counter, or a value a dominating test bounds.\n"
     "\n"
     "Fix: cast one side so the comparison says what it means.\n"
     "    if ((int64)i < (int64)s.length) { ... }\n"},
    {"M0124", "A constant signed operation does not fit its type",
     "An integer literal with no other context is `int32`, and arithmetic\n"
     "on two of them happens in `int32`. When both operands are constants and\n"
     "the exact result does not fit the type, the program gets the wrapped\n"
     "value, whatever the destination is.\n"
     "\n"
     "Example:\n"
     "    var big: int64 = 100000 * 100000;    // M0124: stores 1410065408\n"
     "\n"
     "Fix: widen one operand with a cast so the operation happens in the\n"
     "wider type:\n"
     "\n"
     "    var big: int64 = (int64)100000 * 100000;\n"},
    {"R0001", "A rule gave no usable verdict",
     "A `@rule fn` ran while compiling and did not answer with a verdict the\n"
     "compiler could act on: it trapped, ran out of its step budget, used\n"
     "something the compile-time interpreter cannot run, or returned a site\n"
     "that is not in the Program it was handed.\n"
     "\n"
     "A rule answers with verdict_pass(), verdict_fail(site, message) or\n"
     "verdict_gap(site, message), and every site it names has to be one it\n"
     "read from the Program: a function's or a type's `.site`. The compiler\n"
     "checks that, because a rule is code it does not trust.\n"
     "\n"
     "Fix: return one of the three verdicts, take the site from the\n"
     "Program, and if the rule is expensive raise `--rule-budget=N`.\n"},
    {"L0001", "A metafunction could not run",
     "A decorator names no `@layout` or `@generate` function, or the one it\n"
     "names trapped, ran out of steps, or returned something the compiler\n"
     "could not read. Metafunctions run in the compile-time interpreter\n"
     "before the program is type checked.\n"
     "\n"
     "Fix: import the module that declares the decorator, or fix the function\n"
     "the diagnostic names. `--meta-budget=N` sets its step budget.\n"},
    {"L0002", "A layout failed its proof",
     "The compiler checks every `Layout` before using one. Each field needs\n"
     "one place, no two places may share a bit, each place must fit its\n"
     "region's stride and keep its field's alignment, and a packed value must\n"
     "fit its width. The message names the field that failed.\n"
     "\n"
     "Fix: change the layout function the diagnostic names.\n"},
    {"L0003", "An operation needs an address or a count the layout lacks",
     "A struct with more than one region has no single address per element,\n"
     "so `&a[i]` and indexing a `T*` cannot work. A packed field has no\n"
     "address at all.\n"
     "\n"
     "Fix: take `&a[i].field`, hold the elements in an array or a slice, or\n"
     "copy the value out of the packed field.\n"},
    {"L0004", "A custom layout crossed extern or a kernel",
     "C code and GPU kernels read memory by the default rules, so a struct\n"
     "with a custom layout cannot cross into one.\n"
     "\n"
     "Fix: copy into a struct with the default layout at the boundary.\n"},
    {"L0005", "A generator stated no relation",
     "A `@generate` function wrote code and no `@rule` that checks it against\n"
     "what it replaced, so nothing checks the output. The build goes on and\n"
     "says so.\n"
     "\n"
     "Fix: emit a `@rule` with the generated source.\n"},
    {"L0006", "A metafunction reaches a type whose layout it decides",
     "A metafunction runs before the layouts it computes exist, so the code\n"
     "it reaches cannot use a struct whose layout depends on it. The message\n"
     "prints the chain.\n"
     "\n"
     "Fix: read field names and sizes from the `TypeInfo` it receives.\n"},
    {"L0007", "A pass the program wrote produced code the compiler refused",
     "A `@pass` function runs on the optimizer's IR for a function and\n"
     "writes code back. The compiler checks that code before it replaces\n"
     "the original, and refused it. The message says which check failed: the\n"
     "pass wrote text that does not print, named an operation the IR does\n"
     "not have, broke the shape of the function, or changed what the\n"
     "function does, in which case the counterexample follows.\n"
     "\n"
     "The original function is kept and the build stops.\n"
     "\n"
     "Fix: run the pass on the smaller function the counterexample names and\n"
     "compare the IR it writes with the IR it read.\n"},
    {"R0002", "A rule failed the build",
     "A `@rule fn` the program declares returned verdict_fail for the site\n"
     "the diagnostic points at, with the message it wrote. The build stops\n"
     "here the way `@noalloc` or `@simd!` stops it: the rule is a property\n"
     "the program requires of itself, and this site does not have it.\n"
     "\n"
     "The note names the rule. Read it as ordinary Mettle: it is one, and\n"
     "`mettle expand` prints it with the rest of the program.\n"
     "\n"
     "Fix: change the code the rule points at, or change the rule.\n"},
    {"R0003", "A rule could not decide",
     "A `@rule fn` returned verdict_gap: at this site it could neither\n"
     "prove nor refute the property it checks, and it says so rather than\n"
     "guessing. The build goes on. This is a warning so the gap is loud.\n"
     "\n"
     "A rule reports what it can prove and announces what it cannot, the\n"
     "same standard the borrow analyser is held to. Typical gaps are a call\n"
     "through a function pointer or an extern the rule has no model for.\n"
     "\n"
     "Fix: give the rule what it needs to decide (a direct call, a named\n"
     "function), or accept the gap.\n"},
    {"R0005", "A rule answered differently on a second run",
     "Under `mettle test` every `@rule` is run again over a freshly placed\n"
     "copy of the same program image, and this rule did not give the same\n"
     "verdict twice. A rule is code the compiler runs and does not trust,\n"
     "so a verdict that moves is reported rather than picked from.\n"
     "\n"
     "A rule that is a function of the program it reads answers the same\n"
     "way every time. One that does not is reading something else: an\n"
     "uninitialised local, an address, or an order the compiler is free to\n"
     "change.\n"
     "\n"
     "Fix: make the rule depend only on the Program it was handed.\n"},
    {"R0004", "Rules spent more than their budget",
     "`--rule-budget=N` makes the cost of running the program's rules a\n"
     "contract: the interpreter steps they spend together may not exceed\n"
     "N. They did. `--report-rules` prints what each rule cost.\n"
     "\n"
     "Fix: make the rules cheaper, or raise the budget deliberately.\n"},
    {"P0001", "A declared type's rule could not be proven here",
     "A value flows into a type declared with `type Name = Base where\n"
     "predicate;`, and the compiler could not prove the predicate for it.\n"
     "The message names the conjunct it could not establish, the\n"
     "expression, and the range it knew for it.\n"
     "\n"
     "The proof is the compiler's job and never an annotation. It proves a\n"
     "conversion from a constant, from the value's own type (a uint8 is\n"
     "0..255, a Digit is 0..9), from arithmetic it can bound (`x & 63`,\n"
     "`n % 10`, `a + b` over known ranges), from a loop bound, from a\n"
     "dominating `if` whose condition implies the predicate, from an early\n"
     "exit that rules the other case out, from a call to a function whose\n"
     "body makes the test, from an assignment that stored a value in range,\n"
     "from branches that each stored one, and from a guard that repeats the\n"
     "predicate itself (`if (is_valid(s))` for `where is_valid(value)`).\n"
     "Past that it refuses to guess, which is this error.\n"
     "\n"
     "A test or an assignment proves the value until the next write to it:\n"
     "another assignment, `&name`, a store through a pointer, or a call\n"
     "that may write it. The help line names that write when there is one.\n"
     "Code under `defer` starts with nothing proven, because it runs after\n"
     "the writes below it.\n"
     "\n"
     "A declared type with no predicate (`type Meters = float64;`) is a\n"
     "unit: a plain base value becomes one only by a cast, and two such\n"
     "types never mix in arithmetic.\n"
     "\n"
     "Fix: guard the value where it is converted, or narrow what it is\n"
     "computed from.\n"
     "\n"
     "The predicate speaks about `value` unless the declaration names the\n"
     "binding itself: `type Slot = uint32 where n: n < 64;`.\n"},
    {"F0001", "A function reaches an effect it forbids",
     "A function declared `forbids E` performs E itself or reaches a\n"
     "function that does. The message gives the call chain from the\n"
     "forbidding function to the source, and the notes mark every call on\n"
     "the way and the line that performs the effect.\n"
     "\n"
     "What a function performs is inferred, never annotated in the middle:\n"
     "a function performs what it is declared `with`, and everything its\n"
     "callees perform. `alloc`, `asm` and `syscall` are built in, with\n"
     "`new`, allocator calls, string concatenation, inline assembly and the\n"
     "`syscall` built-in as their sources. A call outside the program with\n"
     "no `with` clause may allocate; a call through a function type with no\n"
     "`with` clause may perform anything, and a forbidding function that\n"
     "reaches one is refused for that reason.\n"
     "\n"
     "Fix: remove the effect from the path, declare the extern `with none`\n"
     "or `with` what it does, or give the function type a `with` clause.\n"},
    {"F0002", "A function requires an effect nothing provides",
     "A function declared `requires E` may only run where E is provided,\n"
     "and the compiler found a way to reach it from a place that provides\n"
     "nothing: `main`, an interrupt handler, a naked function, a kernel, a\n"
     "test, or an exported function of a shared library.\n"
     "\n"
     "A requirement travels up the call graph until a caller declares\n"
     "`provides E`, which grants the effect to everything its body calls.\n"
     "This is how thread affinity, a held lock, or an initialised subsystem\n"
     "becomes something the compiler checks: the function that owns the\n"
     "thread provides the effect, and every function that needs it says so.\n"
     "\n"
     "Fix: call the function from inside something that `provides E`, or\n"
     "make the entry point provide it because it really does.\n"},
    {"F0003", "A function value does not fit the effects its type declares",
     "A function is handed to a function type that declares `with ...` or\n"
     "`requires ...`, and what the function actually performs or requires\n"
     "does not fit. A type with a `with` clause is a closed promise: a value\n"
     "of it performs those effects and nothing else, so a call through it\n"
     "is as good as a direct call. A value with no such clause could be\n"
     "anything, which is why it cannot flow into a type that has one.\n"
     "\n"
     "Passing a function by name lets the compiler check its inferred\n"
     "effects against the type. A value already in a function-typed\n"
     "variable carries only what its own type says.\n"
     "\n"
     "Fix: widen the type's `with` clause, remove the effect from the\n"
     "function, or pass the function by name.\n"},
    {"N0001", "A described machine is not one the compiler can read",
     "A machine is a `const` of `std/machine`'s `MachineInsn`, one row per\n"
     "instruction, and every field of a row is a literal, because all of it\n"
     "is answered before anything runs.\n"
     "\n"
     "Example:\n"
     "    const ISA: MachineInsn[1] = [\n"
     "      { name: \"addi\", encoding: \"10 %0 %1\", operands: 2,\n"
     "        reads: \"%1\", writes: \"%0\", semantics: \"ins_addi\" },\n"
     "    ];\n"
     "\n"
     "An encoding is pairs of hex digits and operand slots, it starts with at\n"
     "least one fixed byte, and no two instructions share their fixed prefix,\n"
     "because that prefix is what a decoder matches on. `reads` and `writes`\n"
     "may only name operands the encoding actually carries. A semantics\n"
     "function takes the three operand slots and returns the next instruction\n"
     "index or -1.\n"},
    {"N0002", "A described machine has no program to run",
     "`mettle emulate` assembles `const PROGRAM: string[N]`, one assembly\n"
     "line per row, into the machine's own encoding. A file describing a\n"
     "machine and nothing to run on it has only half of what is needed.\n"
     "\n"
     "Example:\n"
     "    const PROGRAM: string[2] = [ \"seti 0, 5\", \"out 0\" ];\n"},
    {"N0003", "A line of PROGRAM is not an instruction of this machine",
     "The mnemonic is not one the machine describes, the operand count is\n"
     "not the one the instruction takes, or an operand does not fit the byte\n"
     "the encoding gives it.\n"},
    {"N0004", "A described machine does not round-trip",
     "Assembling and decoding are separate walks over the same description,\n"
     "and re-assembling what the decoder read back is what says the two\n"
     "agree. When they do not, the description is not one machine: it writes\n"
     "bytes it cannot read.\n"
     "\n"
     "The usual cause is two instructions whose fixed prefixes overlap, which\n"
     "is refused up front, or an encoding whose fixed bytes appear where\n"
     "another instruction's operand lands.\n"},
    {"N0005", "A described instruction's semantics did not run",
     "The function an instruction names is what the instruction does, and the\n"
     "compile-time interpreter runs it. This reports that it was not there,\n"
     "that it trapped, or that it ran out of steps.\n"},
    {"N0006", "A described machine did not halt",
     "The emulator runs until the program's bytes run out or an instruction\n"
     "branches past the end. A machine that does neither inside its step\n"
     "budget is reported rather than left running.\n"},
    {"D0001", "A function's longest path costs more than its deadline",
     "`where cycles < N` on a function is a claim about its longest path, and\n"
     "the compiler costs that path from a model of the target: one cost per\n"
     "instruction, a call costed at what the callee's own longest path costs,\n"
     "and a loop costed at its body times a trip count taken from the loop's\n"
     "own bound.\n"
     "\n"
     "Example:\n"
     "    fn tick(seed: int32) -> int32 where cycles < 60 {   // D0001\n"
     "      var i: int32 = 0;\n"
     "      while (i < 8) { seed = mix(seed); i = i + 1; }\n"
     "      return seed;\n"
     "    }\n"
     "\n"
     "`--report-deadlines` prints the path block by block with what each one\n"
     "costs, which is where the work actually is.\n"
     "\n"
     "The model costs the IR as lowered, before the optimizer runs, so the\n"
     "number is an upper bound on the work the program asked for rather than\n"
     "a prediction of a particular machine's cycles.\n"
     "\n"
     "Fixes:\n"
     "  - raise the deadline to what the path costs\n"
     "  - take work off the longest path, which the report names\n"},
    {"D0002", "A function's longest path cannot be bounded",
     "A deadline that cannot be proven is not a deadline. The path runs\n"
     "through something with no bound the compiler can find: a loop whose\n"
     "trip count depends on a value it cannot see, a call through a pointer,\n"
     "a call to a function with no body here, or an asm block.\n"
     "\n"
     "Example:\n"
     "    fn tick() where cycles < 4000 {\n"
     "      while (i < limit) { ... }   // D0002: `limit` is a global\n"
     "    }\n"
     "\n"
     "Fixes:\n"
     "  - bound the loop with a constant, or with a declared type that carries\n"
     "    one, so the trip count is a fact rather than a guess\n"
     "  - build with `--pgo`, which interprets the program and lets a measured\n"
     "    trip count stand in; the deadline then holds on evidence, and\n"
     "    `--report-deadlines` says so in those words\n"
     "  - take the unbounded work out of the function that carries the\n"
     "    deadline\n"},
    {"H0001", "A schedule is not written as phases",
     "A `const` of `std/schedule`'s `Schedule` is read while compiling, so it\n"
     "has to be there to read: an array literal of rows, each naming the\n"
     "phase, the effect that holds while it runs, the function it runs, and\n"
     "the thread it runs on.\n"
     "\n"
     "Example:\n"
     "    const FRAME: Schedule[2] = [\n"
     "      { phase: \"input\", effect: \"Input\", entry: \"read\", thread: 0 },\n"
     "      { phase: \"sim\", effect: \"Sim\", entry: \"step\", thread: 1 },\n"
     "    ];\n"
     "\n"
     "Every field is a literal. Nothing here can be computed at run time,\n"
     "because the dispatcher is generated before the program runs.\n"},
    {"H0002", "A schedule names a phase or an effect twice",
     "Each phase has its own name and its own effect. One effect per phase is\n"
     "what makes a call across a phase boundary something the compiler can\n"
     "refuse: the wrapper for a phase provides that phase's effect and no\n"
     "other, so reaching code that requires a different phase's effect lands\n"
     "somewhere nothing provides it.\n"
     "\n"
     "Two phases sharing an effect would make that boundary invisible.\n"},
    {"H0003", "A schedule names an effect nothing declares",
     "A phase runs under an effect, and the effect has to be declared where\n"
     "the schedule can see it. The compiler does not invent one, because an\n"
     "effect a program never wrote is one it cannot say anything about.\n"
     "\n"
     "Fix: `effect <name>;` beside the schedule.\n"},
    {"H0004", "A schedule names an entry nothing declares",
     "A phase runs a function of this module, named as it was written. The\n"
     "name is a string in the schedule, so a typo is a name that resolves to\n"
     "nothing rather than a call that does not compile; this is the check\n"
     "that turns the first into the second.\n"},
    {"H0005", "A schedule is a `var`",
     "A schedule is read while compiling, so it has to be a `const`. A `var`\n"
     "can change while the program runs, and the dispatcher was generated\n"
     "before it started.\n"},
    {"H0007", "A phase joins the threads and there is nothing to build it out of",
     "`joins: true` on a phase says every thread waits at that phase's\n"
     "boundary before any of them starts the next frame. The wait is\n"
     "generated out of `std/thread`'s atomics, so the module has to import\n"
     "them.\n"
     "\n"
     "Fix: `import \"std/thread\";` beside the schedule.\n"
     "\n"
     "The counter behind a join only ever rises: a thread arriving for frame\n"
     "g leaves it at g times the number of threads, so there is nothing to\n"
     "reset and nothing to race over. It is `volatile`, so the spin reads\n"
     "memory every time round.\n"},
    {"H0006", "The dispatcher generated from a schedule did not parse",
     "This is a compiler fault rather than a program's. The generated\n"
     "dispatcher is ordinary Mettle and `mettle expand` prints it; if that\n"
     "text is not valid, the generator produced something wrong. Please\n"
     "report it with the schedule that caused it.\n"},
    {"F0006", "Two threads write the same global and nothing orders them",
     "Two functions write one global, and the effects each needs place them\n"
     "somewhere different: one runs where an effect it requires is provided,\n"
     "the other where a different one is, and no effect appears in both\n"
     "requirement sets. Nothing in the program says which write lands last.\n"
     "\n"
     "Example:\n"
     "    effect Sim;\n"
     "    effect Render;\n"
     "    var frame: int32 = 0;\n"
     "    fn tick() requires Sim { frame = frame + 1; }   // F0006\n"
     "    fn draw() requires Render { frame = 0; }\n"
     "\n"
     "An effect BOTH writers require is what says they are ordered, because\n"
     "whoever provides it runs them one at a time. That is what a lock is,\n"
     "spelled as a requirement:\n"
     "\n"
     "    effect FrameLock;\n"
     "    fn tick() requires Sim, FrameLock { frame = frame + 1; }\n"
     "    fn draw() requires Render, FrameLock { frame = 0; }\n"
     "\n"
     "Fixes:\n"
     "  - give both writers a requirement they share, and provide it around\n"
     "    each write with the lock the program already holds\n"
     "  - give each thread its own copy and combine them at a point one\n"
     "    thread owns\n"
     "  - move one of the writes to where the other one runs\n"
     "\n"
     "Nothing is said about a global whose writers need no placed effect at\n"
     "all. A program that declares no `provides` is not making a claim about\n"
     "where its code runs, and this check makes none about it.\n"},
    {"F0005", "The effect pass spent more than its budget",
     "`--effect-budget=N` makes the cost of inferring the program's effects\n"
     "a contract: the steps the pass spends scanning bodies and running its\n"
     "fixpoint may not exceed N. They did.\n"
     "\n"
     "`--report-effects` prints what the pass settled, one line per\n"
     "function that performs or needs anything, and the totals: functions\n"
     "seen, functions with an effect, fixpoint rounds, steps.\n"
     "\n"
     "Fix: declare fewer effects, cut the call graph the fixpoint walks, or\n"
     "raise the budget deliberately.\n"},
    {"T0001", "A function and its reference twin disagree",
     "`fn fast(...) -> R reference slow;` says the two compute the same\n"
     "thing, and the build checks it. The differential prober generates\n"
     "input sets from the parameter shapes plus the constants both\n"
     "functions compare against, runs both in the interpreter, and this\n"
     "pair disagreed. The message carries the input that shows it.\n"
     "\n"
     "This is a differential test and it says so: agreement is evidence,\n"
     "not equivalence. Disagreement is decisive, because one input where\n"
     "they differ is all a counterexample needs.\n"
     "\n"
     "The same check runs again after the optimizer under `--verify`, so a\n"
     "pass that breaks the fast one is caught by the reference the program\n"
     "already supplied.\n"
     "\n"
     "Fix: make them agree, or stop claiming they do.\n"},
    {"T0002", "A reference twin could not be checked",
     "The pair was declared and the prober could not generate inputs it\n"
     "could run: a parameter shape it has no values for, or a construct\n"
     "the interpreter cannot execute. The build goes on and this warning\n"
     "stands, because a check that did not run must not read as one that\n"
     "passed.\n"
     "\n"
     "`--report-twins` prints what each pair was checked on, so a pair that\n"
     "was checked on nothing is visible beside the pairs that were.\n"
     "\n"
     "Fix: narrow the parameters to shapes the prober can build, or accept\n"
     "that this pair is unchecked and know that it is.\n"},
    {"C0001", "Two kernels of a numerics contract compute an output differently",
     "Every kernel marked @numerics(C) promises the same bits for each output\n"
     "element as every other member of C, given the same inputs. The build\n"
     "proves it: each harness of C runs in the compile-time interpreter with\n"
     "every input byte a symbol, so each output element comes out as its\n"
     "computation, the ordered chain of rounding operations and where every\n"
     "operand came from. Two members that agree compute the same chain.\n"
     "\n"
     "These two do not. The message names the element, the claim\n"
     "(numerics_same) that compared it, and the first operation where the\n"
     "chains part, with its line in each kernel. For an MMA step it names the\n"
     "A or B element that differs; a sum of the same terms added in another\n"
     "order is named as that sum. A different rounding order,\n"
     "an extra operation (x + 0 * v is not a no-op: v can be infinite, and\n"
     "the sum can lose a negative zero) or a different operand all part the\n"
     "chains.\n"
     "\n"
     "Fix: make the kernels compute the same thing, or take the kernel out of\n"
     "the contract.\n"},
    {"C0002", "The numerics check cannot follow a computation",
     "The check follows integer and floating-point arithmetic, conversions,\n"
     "loads and stores, copies, subgroup operations, tensor MMAs and the math\n"
     "intrinsics on values computed from the contract's inputs. It stops, and\n"
     "the build fails, where it cannot decide what happens: a branch, address\n"
     "or loop bound computed from the inputs (a branch whose arms only\n"
     "compute values is followed as a select), an operation with no meaning\n"
     "here such as inline asm or an extern call, a read of bytes an\n"
     "asynchronous copy has not delivered, or an interpreter limit. The\n"
     "message names the line.\n"
     "\n"
     "Fix: compute the decision from indices and sizes, which the check\n"
     "knows, or write the data-dependent choice as an assignment in both\n"
     "arms.\n"},
    {"C0003", "A numerics contract is unproven for a kernel",
     "A kernel that declares @numerics(C) is a member of C, and the build\n"
     "proves C by running its harnesses. A member that no harness launches,\n"
     "or a harness that makes no claim with numerics_same, leaves the\n"
     "contract unproven, and an unproven contract is not stated.\n"
     "\n"
     "Fix: launch the member from a harness of C and compare its output with\n"
     "another member's.\n"},
    {"C0004", "A kernel walks K in an order its contract does not allow",
     "A contract with k_order: k_ascending promises that every tensor\n"
     "accumulation visits K in ascending order, one chain from the first\n"
     "block to the last. The check reads the order off each output's chain:\n"
     "which input bytes each MMA step's B operand came from. A member that\n"
     "reorders the blocks, or splits K into partial sums added afterwards,\n"
     "rounds differently from one that walks K once in order.\n"
     "\n"
     "Fix: accumulate the blocks in ascending order into one accumulator.\n"},
    {"G0001", "A kernel holds more tile registers at once than its budget",
     "A register tile is a subgroup's value held in registers, each work\n"
     "item its share, and tiles never spill. The backend states what each\n"
     "tile costs a work item (PTX: an f32 fragment_c M x N tile costs\n"
     "M*N/32 registers, an f16 fragment_a M x K tile M*K/64, a row vector\n"
     "M/8), takes the tiles live at each point of the kernel, and compares\n"
     "the peak with the work item's register limit: 255, or the multiple\n"
     "of 8 below 65536 / block threads when that is smaller. This kernel\n"
     "went over. The message names the line of the peak and every tile\n"
     "live there with its cost.\n"
     "\n"
     "A run of element-wise tile operations is computed one element at a\n"
     "time, so a tile made inside such a run and used only there costs\n"
     "nothing. `--explain` and `--report-gpu-types` print every tile, its\n"
     "cost, its live lines and the peak.\n"
     "\n"
     "Fix: shrink a tile, end one's life earlier, or keep an operand in\n"
     "memory and let tensor_mma read it there.\n"},
    {"G0002", "ptxas spilled a kernel that holds register tiles",
     "The tiles fit the compiler's own budget, and when ptxas is on PATH\n"
     "the build also assembles the module and reads what the assembler\n"
     "did. It spilled this kernel: its tiles plus everything else live\n"
     "beside them did not fit. The build fails, because a tile that does\n"
     "not stay in registers is not what the program asked for.\n"
     "\n"
     "Without ptxas on PATH the build says the tiles are unconfirmed and\n"
     "goes on, so the gap is stated.\n"
     "\n"
     "Fix: shrink a tile or what is live beside it.\n"},
    {"F0004", "A function declared @pure performs something",
     "`@pure` is a contract, and this build checked it. The function\n"
     "carries the decorator and its body, or something it calls, writes\n"
     "observable state: a global, memory through a pointer, an allocation,\n"
     "inline assembly, or a call the compiler cannot see into.\n"
     "\n"
     "The decorator buys no optimization. Purity is inferred by a\n"
     "whole-program fixpoint, and the loop-invariant call hoist reads only\n"
     "what that pass proved, so a pure function has its call hoisted\n"
     "whether or not anyone wrote `@pure` on it. What the decorator does is\n"
     "make the claim checkable: write it down and the build fails the day\n"
     "the body stops being pure.\n"
     "\n"
     "Fix: remove the write, or remove the decorator.\n"},
    {"P0003", "The declared-type prover spent more than its budget",
     "`--proof-budget=N` makes the cost of proving declared types a\n"
     "contract: the interval and guard steps the prover spends may not\n"
     "exceed N. They did.\n"
     "\n"
     "`--report-proofs` prints one line per conversion the prover settled:\n"
     "the type, the expression, the site, whether it was proven, what it\n"
     "cost, and which route settled it (a range, or a dominating test).\n"
     "\n"
     "Fix: declare fewer refined types on hot paths, narrow what they are\n"
     "computed from so the prover settles sooner, or raise the budget\n"
     "deliberately.\n"},
    {"P0004", "The declared-type prover stopped at its step ceiling",
     "The prover that settles declared types stops after a fixed number of\n"
     "steps so one hard predicate cannot stall a build. Past that point it\n"
     "answers every remaining question as unknown, and an unknown proof\n"
     "refuses exactly as a false one does.\n"
     "\n"
     "`--report-proofs` prints what each proof cost.\n"
     "\n"
     "Fix: narrow the values a declared type is computed from so the prover\n"
     "settles sooner, or split the function that carries the proofs.\n"},
    {"V0001", "A trust variable makes the build record what it did not check",
     "METTLE_TRUST_REFINEMENTS, METTLE_TRUST_EFFECTS and\n"
     "METTLE_TRUST_DEADLINES switch off a family of checks: declared-type\n"
     "proofs, effect checks, or deadline checks. A build with one set\n"
     "records those properties as established without establishing them,\n"
     "and prints this warning so the record is not mistaken for a proof.\n"
     "\n"
     "Fix: unset the variable to get the checked build.\n"},
    {"P0002", "A bounds check was proven away by a declared type",
     "Not an error. `--explain` reports it: an index whose declared type\n"
     "pins its range inside the array it indexes needs no bounds check, so\n"
     "none was emitted, in debug, release and --safe builds alike. The\n"
     "proof is the type's predicate; the pass that consumed it is lowering,\n"
     "which decides check emission per access.\n"
     "\n"
     "A property the program declared and the compiler proved earns the\n"
     "same treatment as one the compiler discovered on its own, and no\n"
     "more: an index of a plain integer type keeps its check.\n"
     "\n"
     "The prover is code, so it is not believed on its own authority.\n"
     "`mettle test`, `mettle trace` and `--verify` re-check every proven\n"
     "conversion as the interpreter runs, and `--check-proofs` does the\n"
     "same inside a compiled program, in `--release` too.\n"},
    {"O0001", "Use of a value after it moved",
     "A value that owns a resource moved: into a by-value parameter, a\n"
     "return, a variable, a field or a drop. After a move the old name no\n"
     "longer holds anything, so reading it, borrowing it or moving it again\n"
     "is an error. \"On some paths\" means one branch moved it and another\n"
     "did not.\n"
     "\n"
     "    var a = open(\"log\");\n"
     "    consume(a);\n"
     "    write(&a, \"x\");    // a moved into consume\n"
     "\n"
     "Fix: pass a pointer (`&a`) to a function that only needs to look, or\n"
     "put a new value in `a` before using it again.\n"},
    {"O0002", "Use of a value with a part moved out",
     "A field of a struct moved out on its own. The rest of the struct is\n"
     "still there and is dropped at the end of its scope, but the struct\n"
     "as a whole cannot be used, borrowed or moved.\n"
     "\n"
     "Fix: use the fields that are still there, or assign the moved field\n"
     "a new value first.\n"},
    {"O0003", "Moving a field out of a type with a destructor",
     "A struct with its own `drop` method gets the whole value when it is\n"
     "dropped, so none of its fields can be moved out or dropped on their\n"
     "own.\n"
     "\n"
     "Fix: `mem_take(&s.field)` moves the field out and leaves a zero value\n"
     "behind, and `mem_replace(&s.field, value)` leaves the value you give\n"
     "it.\n"},
    {"O0004", "A move that only happens sometimes inside an expression",
     "The right side of `&&` and `||` runs only when the left side lets it.\n"
     "A move there would leave the value moved on some runs and not others\n"
     "in the middle of one expression.\n"
     "\n"
     "Fix: write it as an `if` statement, so each branch moves or keeps the\n"
     "value and the compiler can follow both.\n"},
    {"O0005", "errdefer that drops or moves an owned value",
     "An owned value is dropped at the end of its scope on every path out,\n"
     "an error return included. An `errdefer` that drops or moves it again\n"
     "would drop it twice.\n"
     "\n"
     "Fix: remove the errdefer; the value is already handled.\n"},
    {"O0006", "Assigning into part of a moved value",
     "The whole struct moved away, so there is nothing to assign a field\n"
     "of.\n"
     "\n"
     "Fix: assign the whole struct first.\n"},
    {"O0007", "Calling a destructor by name",
     "`T_drop__ptr` runs only the type's own cleanup. Calling it leaves the\n"
     "fields undropped and leaves the compiler thinking the value is still\n"
     "alive, so it would be dropped again at the end of its scope.\n"
     "\n"
     "Fix: write `drop(value)`. It runs the destructor, drops the fields,\n"
     "and marks the value gone.\n"},
    {"O0008", "Moving out of a global",
     "A global lives for the whole run and is never dropped, so a value\n"
     "cannot be moved out of it and leave nothing behind.\n"
     "\n"
     "Fix: `mem_take(&global)` or `mem_replace(&global, value)`.\n"},
    {"O0009", "Moving out of memory reached through a pointer or index",
     "The compiler follows locals, parameters and their fields. Memory\n"
     "behind a pointer or inside an array element belongs to something\n"
     "else that will drop it, so moving out of it would drop it twice.\n"
     "\n"
     "Fix: `mem_take(p)` moves the value out and leaves zero behind;\n"
     "`mem_replace(p, value)` leaves the value you give it.\n"},
    {"O0010", "An owning value declared in a loop header",
     "A `for` header has no block of its own to drop the value at the end\n"
     "of.\n"
     "\n"
     "Fix: declare it before the loop.\n"},
    {"O0011", "A destructor with the wrong shape",
     "A destructor is the method `drop` with a pointer receiver:\n"
     "`fn T_drop__ptr(self: T*)` or `method drop()` inside the struct. It\n"
     "takes nothing else and returns nothing. A by-value `T_drop` would\n"
     "receive its own copy and drop that, forever.\n"},
    {"O0012", "A function named drop or forget",
     "`drop(x)` and `forget(x)` are built in. `drop` runs a value's\n"
     "destructor and its fields' destructors and marks it gone; `forget`\n"
     "marks it gone and runs nothing. The compiler inserts calls to `drop`\n"
     "itself, so the name cannot mean anything else.\n"
     "\n"
     "Fix: rename the function.\n"},
    {"O0013", "A closure that captures an owned value",
     "A closure copies what it captures into a heap environment, and that\n"
     "environment is never dropped. A captured owning value would be\n"
     "dropped where it was declared while the closure still held a copy.\n"
     "\n"
     "Fix: capture a pointer to it: `var held = &name;` before the lambda,\n"
     "and `*held` inside.\n"},
    {"O0014", "An owned value passed to a gathered parameter",
     "A last parameter written `T[..]` collects the extra arguments into a\n"
     "slice. A slice owns nothing, so an owning value passed there would\n"
     "never be dropped.\n"
     "\n"
     "Fix: pass pointers, or give the function a parameter of the owning\n"
     "type.\n"},
    {"O0015", "A drop of a string the function did not build",
     "A string is a view. It owns its bytes only when interpolation, `+`,\n"
     "or a function that returns a fresh string built it in this function.\n"
     "A literal, a parameter, a field or a string another variable kept is\n"
     "someone else's, and freeing it would break whoever owns it.\n"
     "\n"
     "Fix: remove the drop. Strings this function built are dropped for it\n"
     "at the end of their scope; `mettle expand` shows where.\n"},
    {"O0020", "`@explicit` code leaves an owned value undropped",
     "In an `@explicit` function the compiler inserts no drops. A value\n"
     "that still owns a resource when its scope ends would leak, so it is\n"
     "an error, and the message says what the compiler would have written.\n"
     "\n"
     "Fix: `defer drop(x);` after the declaration drops it at the end of\n"
     "its scope unless it moved, which is what implicit code does.\n"
     "Dropping or moving it on every path works as well. `--fix` writes\n"
     "the defer.\n"},
    {"O0021", "`@explicit` assignment over an owned value",
     "Assigning to a variable that still owns a resource would lose the\n"
     "old value without dropping it.\n"
     "\n"
     "Fix: drop the old value first. Implicit code evaluates the new value,\n"
     "then drops the old one, then stores; `--fix` writes it that way.\n"},
    {"O0022", "`@explicit` code throws an owned value away",
     "A call returns a value that owns a resource and nothing keeps it.\n"
     "\n"
     "Fix: `drop(f());`, or keep the value. `--fix` writes the drop.\n"},
    {"O0023", "`@explicit` code uses an owned temporary",
     "A value that owns a resource is used without a name, as in\n"
     "`open(path).size` or `println(\"n = {n}\")`, whose interpolation\n"
     "builds a string on the heap. Implicit code holds it in a hidden local\n"
     "until the statement ends and drops it there.\n"
     "\n"
     "Fix: put it in a variable, use it, and drop it. `--fix` writes that\n"
     "when the value sits in a statement.\n"},
    {"O0099", "Internal ownership error",
     "The drops the compiler inserted into a function did not account for\n"
     "every owned value when it checked its own work. This is a compiler\n"
     "bug; the build stops so the program never runs with a missing or\n"
     "doubled drop. `mettle expand` shows what was inserted.\n"},
};

typedef enum {
  DECISION_VECTOR_REFUSAL,
  DECISION_INLINE_REFUSAL,
  DECISION_APPLIED,
  DECISION_DEVICE
} DecisionGroup;

typedef struct {
  const char *code;
  DecisionGroup group;
  const char *title;
  const char *body;
} DecisionDoc;

static const DecisionDoc DECISIONS[] = {
    {"call-in-body", DECISION_VECTOR_REFUSAL,
     "The loop body calls a function",
     "A SIMD kernel runs eight lanes at once. A call runs one. So a loop that\n"
     "calls anything on every iteration cannot become a kernel until the call\n"
     "is gone.\n"
     "\n"
     "The inliner runs first and removes most of these on its own, so a loop\n"
     "that still reports this one has a call the inliner declined. The remark\n"
     "for that call, on the same line, says which rule stopped it.\n"
     "\n"
     "Fix: mark the callee @inline, or lift the call out of the loop when its\n"
     "result does not change between iterations.\n"},
    {"extern-call-in-body", DECISION_VECTOR_REFUSAL,
     "The loop body calls an external function",
     "The body calls a function declared `extern`. The compiler has no body\n"
     "for it, so it can never inline the call away, and the loop can never\n"
     "vectorize while the call is there.\n"
     "\n"
     "Fix: move the extern call out of the loop. Where the loop computes the\n"
     "values and the extern call only consumes them, split it in two: a\n"
     "vectorizable loop that fills a buffer, then one call.\n"},
    {"memory-safety-in-body", DECISION_VECTOR_REFUSAL,
     "The loop retains a memory safety operation",
     "The compiler recognizes the check or pointer origin operation, but\n"
     "cannot prove that it can move out of this loop or be removed.\n"
     "A vector kernel must preserve that operation and the failing access.\n"
     "\n"
     "A fixed array extent and an affine index can expose a proof. If the\n"
     "index or the allocation lifetime varies, keep the checked scalar loop.\n"},
    {"indirect-call", DECISION_VECTOR_REFUSAL,
     "The loop body calls through a function pointer",
     "The callee is not known until the loop runs, so there is nothing to\n"
     "inline and no kernel to match.\n"
     "\n"
     "Fix: where the pointer holds the same function for the whole loop,\n"
     "switch on it once outside and write a direct call in each arm. Each arm\n"
     "then vectorizes on its own.\n"},
    {"alloc-in-body", DECISION_VECTOR_REFUSAL,
     "The loop body allocates",
     "Every iteration runs `new`. Allocation is a call into the allocator, it\n"
     "can fail, and it has an order the lanes would have to keep.\n"
     "\n"
     "Fix: allocate once before the loop and reuse the buffer.\n"},
    {"inline-asm", DECISION_VECTOR_REFUSAL,
     "The loop body contains inline assembly",
     "The compiler treats an asm block as opaque. It cannot know which\n"
     "registers or memory the block touches, so it cannot prove the lanes are\n"
     "independent.\n"
     "\n"
     "Fix: move the asm out of the loop, or accept the scalar loop. Hand-written\n"
     "asm in a hot loop is usually already the vector code you wanted.\n"},
    {"control-flow", DECISION_VECTOR_REFUSAL,
     "The loop body branches on data",
     "The body has a data-dependent `if`. The lane vectorizer runs an\n"
     "if/else as masks: both arms compute in every lane and a select keeps\n"
     "the right one. It takes that shape only inside a loop it can run at\n"
     "all, and the remark says where it stopped:\n"
     "\n"
     "  - the loop test is not a signed `counter < bound`\n"
     "  - the counter does not step by one as the last statement\n"
     "  - an element is not 32 bits wide\n"
     "  - an address is not `base[i]` with the counter as the index\n"
     "\n"
     "The fix line under the remark is the fix for that stop.\n"},
    {"early-exit", DECISION_VECTOR_REFUSAL,
     "The loop can leave before its trip count",
     "A `break` or an early `return` means the number of iterations is not\n"
     "known when the loop starts. A kernel that processes eight elements per\n"
     "step could run past the element the loop meant to stop at.\n"
     "\n"
     "Fix: where the search and the work are separable, do the work in a\n"
     "counted loop and the search in its own. A `break` on the last iteration\n"
     "costs nothing to keep scalar.\n"},
    {"int16-elements", DECISION_VECTOR_REFUSAL,
     "16-bit integer elements have no kernel",
     "The loop loads or stores int16 or uint16. Mettle has kernels for 8-bit\n"
     "and 32-bit integers and for both float widths, and none for 16-bit.\n"
     "\n"
     "Fix: widen the array to int32. The loop then runs eight lanes at a time\n"
     "where it ran one, which more than pays for the extra memory in a hot\n"
     "loop. Where the values fit in a byte, int8 is faster still.\n"
     "\n"
     "The missing kernel is the compiler's gap; the loop is fine. The report\n"
     "simulates the widening and names the kernel you would get.\n"},
    {"int64-elements", DECISION_VECTOR_REFUSAL,
     "64-bit integer elements have no kernel",
     "The loop loads or stores int64 or uint64. There is no 64-bit integer kernel\n"
     "yet, so the loop stays scalar.\n"
     "\n"
     "Fix: use int32 where the range allows it. Four lanes per vector become\n"
     "eight. Where the values genuinely need 64 bits, this loop is at its\n"
     "floor for now.\n"},
    {"reloaded-base", DECISION_VECTOR_REFUSAL,
     "The base pointer is re-read every iteration",
     "The array the loop indexes is reached through a pointer stored in a\n"
     "struct: `t->counts[i] = 0`. The body writes through that pointer, and\n"
     "nothing rules out the write landing on the pointer field itself, so the\n"
     "compiler must re-read `t->counts` on every iteration. A kernel needs one\n"
     "base for the whole loop, so the loop stays scalar.\n"
     "\n"
     "Fix: read the pointer once into a local and index the local.\n"
     "\n"
     "    var counts: int32* = t->counts;\n"
     "    while (i < n) { counts[i] = 0; i += 1; }\n"
     "\n"
     "The local says the base does not change, which is what the loop meant.\n"
     "The compiler's alias reasoning cannot prove that on its own yet; the\n"
     "code was right all along.\n"},
    {"serial-recurrence", DECISION_VECTOR_REFUSAL,
     "The loop carries a serial recurrence",
     "A value is computed from its own previous value through an operation\n"
     "that does not reassociate: `*`, `/`, a shift, or a bitwise operation.\n"
     "Iteration 2 cannot start until iteration 1 has finished, so the work is\n"
     "one chain and there is nothing independent to spread across lanes.\n"
     "\n"
     "This is the one refusal that comes from the algorithm itself. In a hash,\n"
     "a linear congruential generator or an IIR filter the running state is\n"
     "the algorithm, and the loop is already at its scalar floor.\n"
     "\n"
     "What does vectorize: `+` and `-` reductions. Those reassociate, so the\n"
     "compiler splits them into per-lane partial sums and adds the lanes at\n"
     "the end.\n"
     "\n"
     "Fix: none, unless the algorithm itself can change. Some hashes have a\n"
     "parallel form that hashes N independent streams and combines them.\n"},
    {"mixed-float-widths", DECISION_VECTOR_REFUSAL,
     "The loop mixes float32 and float64",
     "One vector register holds eight float32 or four float64. A loop that\n"
     "touches both would need two lane counts at once, and the conversion\n"
     "between them costs more than the kernel saves.\n"
     "\n"
     "Fix: pick one width for the whole loop. Convert on the way in or on the\n"
     "way out, outside the loop.\n"},
    {"byte-sum-narrow-acc", DECISION_VECTOR_REFUSAL,
     "Byte sum into an accumulator narrower than int64",
     "Summing bytes has a very fast kernel (`vpsadbw`, 32 bytes per step), but\n"
     "it accumulates into int64: 32 lanes of a byte each can carry further\n"
     "than 32 bits, and the kernel will not silently give you a different\n"
     "answer from the scalar loop.\n"
     "\n"
     "Example:\n"
     "    var total: int64 = 0;\n"
     "    for i in 0..n {\n"
     "        total = total + (int64)data[i];\n"
     "    }\n"
     "\n"
     "Fix: declare the accumulator int64. The report simulates that change,\n"
     "re-runs the optimizer, and only prints the advice once the loop really\n"
     "does vectorize with it.\n"},
    {"int32-sum-narrow-acc", DECISION_VECTOR_REFUSAL,
     "int32 sum into an accumulator narrower than int64",
     "Same rule as `byte-sum-narrow-acc`, one width up. The int32 sum kernel\n"
     "sign-extends each element to 64 bits with `vpmovsxdq` and adds in 64-bit\n"
     "lanes with `vpaddq`, so its total is an int64, and it is written for an\n"
     "int64 accumulator.\n"
     "\n"
     "Fix: declare the accumulator int64. The elements stay int32; only the\n"
     "running total widens.\n"},
    {"inlined-param-local", DECISION_VECTOR_REFUSAL,
     "An inlined parameter copy survived in the body",
     "The inliner left a `__inl_*` temporary inside the loop, and the\n"
     "recognizers see a local write per iteration where they expected a plain\n"
     "array access.\n"
     "\n"
     "The compiler cannot see through that temporary yet, and the code is\n"
     "fine. Report the loop if you hit it; the shape that produced it is worth\n"
     "fixing.\n"},
    {"body-local", DECISION_VECTOR_REFUSAL,
     "A local is declared inside the loop body",
     "The recognizers match on a straight run of loads, arithmetic and stores.\n"
     "A local declared in the body adds a stack slot written every iteration,\n"
     "which none of the shapes cover.\n"
     "\n"
     "Fix: declare the variable before the loop and assign it inside, or fold\n"
     "the expression into the statement that uses it.\n"},
    {"dot-shape-address", DECISION_VECTOR_REFUSAL,
     "Dot product with an address the kernel cannot follow",
     "The loop is a float multiply and accumulate, which is the FMA dot\n"
     "product kernel's shape, but the addresses do not match. The kernel needs\n"
     "each base to be a plain pointer indexed by the loop counter, `a[i]`;\n"
     "`a[r * cols + i]` does not qualify.\n"
     "\n"
     "Example, an inner product over one row of a matrix, with the row's base\n"
     "taken once outside the inner loop:\n"
     "    for r in 0..rows {\n"
     "        var row: float32* = &m[r * cols];\n"
     "        for c in 0..cols {\n"
     "            acc = acc + row[c] * x[c];\n"
     "        }\n"
     "    }\n"
     "\n"
     "Fix: lift the invariant part of the index into a pointer before the loop.\n"
     "The report checks first: when the other half of the index changes every\n"
     "iteration the access really strides, and the report says so and gives\n"
     "no advice.\n"},
    {"lane-operation", DECISION_VECTOR_REFUSAL,
     "An operation the 32-bit lanes cannot do exactly",
     "The lane vectorizer runs any counted loop over 32-bit elements as eight\n"
     "lanes of float32, int32 or uint32, including branches, selects,\n"
     "conversions and bit casts. It stopped at one operation, which the\n"
     "remark names by line:\n"
     "\n"
     "  - Arithmetic in float64. Lanes are 32 bits wide.\n"
     "  - Integer division or remainder. AVX2 has no integer divide and no\n"
     "    multiply-high to build a constant divide from.\n"
     "  - A value whose full 64 bits matter, such as an int64 compared or\n"
     "    shifted right. Lanes keep the low 32 bits.\n"
     "  - A shift by more than 31, or by a count that may be negative.\n"
     "  - A constant or a variable with no 32-bit lane form.\n"
     "  - Pointer arithmetic other than `base[i]`, such as a struct field\n"
     "    `b[i].x` or an address taken with `&a[i]`.\n"
     "\n"
     "Adding, subtracting and multiplying int64 values is not on the list:\n"
     "the lanes compute the low 32 bits exactly, and a store into a 32-bit\n"
     "element keeps only those.\n"},
    {"scalar-target", DECISION_VECTOR_REFUSAL,
     "A target this compiler emits no vector code for",
     "The build targets AArch64. Mettle's loop vectorizers write x86-64 SSE\n"
     "and AVX2, and the AArch64 backend has no NEON code generation, so every\n"
     "loop runs as scalar AArch64 code. The target-neutral optimizer still\n"
     "runs: inlining, invariant code motion, strength reduction and the SSA\n"
     "passes.\n"
     "\n"
     "An @simd! loop fails the build on this target for the same reason.\n"
     "Build the same file for x86_64 with --explain to see whether its\n"
     "vectorizers take the loop.\n"},
    {"carried-value", DECISION_VECTOR_REFUSAL,
     "A value carried from one iteration into the next",
     "Lanes run eight iterations side by side, so no iteration may read what\n"
     "the one before it wrote, except through a running sum, which regroups.\n"
     "The remark names the value and how it is carried:\n"
     "\n"
     "  - Through an operation other than + or -, as in a hash or a filter.\n"
     "    That chain is the algorithm, and the loop is at its scalar floor.\n"
     "  - As the previous element, `prev = x[i]` read on the next pass. Read\n"
     "    `x[i - 1]` from the array instead.\n"
     "  - As the value from the last iteration whose condition held. Find\n"
     "    that index in a loop of its own.\n"
     "  - As a sum wider than 32 bits. Accumulate in int32 or float32 and\n"
     "    widen after the loop.\n"
     "  - As a value the next iteration reads before writing, with no running\n"
     "    sum to regroup: a pointer chase `i = next[i]`, or Fibonacci's\n"
     "    `a, b = b, a + b`. That chain is the algorithm.\n"
     "\n"
     "A conditional sum, where each arm of an `if` adds to the total, is a\n"
     "running sum and vectorizes.\n"},
    {"lane-budget", DECISION_VECTOR_REFUSAL,
     "More operands than the lane kernel stages",
     "The lane kernel takes its arrays, loop-invariant values, sums and loop\n"
     "bounds in at most 16 staged operands. A body over many arrays runs out.\n"
     "\n"
     "Fix: split the body into two loops over the same range, each over fewer\n"
     "arrays.\n"},
    {"store-only-fill", DECISION_VECTOR_REFUSAL,
     "Fill loop the fill kernel could not claim",
     "The loop writes the same value over and over, which is the fill shape,\n"
     "but one of the kernel's requirements was not met. The remark names\n"
     "which, because the answer differs:\n"
     "\n"
     "  - Several stores in one body. The kernel fills one region per loop.\n"
     "    Split it into one loop per destination and each becomes a kernel.\n"
     "\n"
     "  - 1-byte elements. The kernel covers 2, 4 and 8 bytes. This is a gap\n"
     "    in the compiler; there is nothing to change in the loop.\n"
     "\n"
     "  - An address the kernel cannot follow. It handles `a[i]`, `a[c + i]`\n"
     "    with `c` invariant, and a pointer walked by a constant stride. Lift\n"
     "    the invariant part of the index into a base pointer before the loop.\n"},
    {"counter-store", DECISION_VECTOR_REFUSAL,
     "A ramp: the loop stores its own counter",
     "`a[i] = i`, `xs[i] = (float32)i` and every other store of a value\n"
     "computed from the loop counter is a ramp. The lane vectorizer builds\n"
     "ramps from a vector of counters over 32-bit elements when the counter\n"
     "steps by one up to a bound. A ramp into 8-bit or 16-bit elements, or\n"
     "one whose counter moves another way, stays a scalar loop.\n"
     "\n"
     "A store of a value that does not change, `a[i] = 0`, is a fill and\n"
     "becomes 16- or 32-byte splat stores, on a stack array as well as\n"
     "through a pointer.\n"},
    {"scatter-store", DECISION_VECTOR_REFUSAL,
     "A store whose index the loop reads from memory",
     "When the element a store writes is chosen by data, `heat[cell] = ...`\n"
     "with `cell` read from an array, the store is a scatter. AVX2 can\n"
     "gather loads but has no scatter store, so a vector body would still\n"
     "store one lane at a time.\n"
     "\n"
     "The commonest case is a histogram, `h[x[i]] = h[x[i]] + 1`. Two lanes\n"
     "of one vector can pick the same element, and a vector update would\n"
     "keep only one of the two increments. AVX-512 detects that collision\n"
     "with vpconflictd; AVX2 cannot, and the scalar loop is the floor.\n"},
    {"invariant-addend", DECISION_VECTOR_REFUSAL,
     "A sum that also adds the same term every iteration",
     "`s = s + a[i] + bias(k)`, with `s` an int64 and `a` int32, sums the\n"
     "elements and adds `bias(k)` on every iteration. The term does not\n"
     "change inside the loop, and the compiler hoists the call, but the\n"
     "widening sum kernel adds one element per lane and nothing else, and\n"
     "the 32-bit lanes cannot hold an int64 total, so the loop stays scalar.\n"
     "An int32 `s` would vectorize as written, and could overflow.\n"
     "\n"
     "Fix: sum the elements alone and add the term once:\n"
     "\n"
     "    var s: int64 = 0;\n"
     "    for i in 0..n { s = s + a[i]; }\n"
     "    s = s + (int64)n * bias(k);\n"
     "\n"
     "The loop then runs as vpmovsxdq and vpaddq, eight elements a pass.\n"},
    {"chosen-index", DECISION_VECTOR_REFUSAL,
     "The condition reads at an index the loop chose",
     "`if (a[i] > a[best]) { best = i; }` finds the index of the largest\n"
     "element, but it reads `a[best]` in the comparison, and `best` is set\n"
     "inside the `if`. Every iteration's load waits on the previous\n"
     "iteration's choice, so the iterations form a chain.\n"
     "\n"
     "Fix: keep the best value in a variable of its own beside its index:\n"
     "\n"
     "    var best_value = a[0];\n"
     "    for i in 0..n {\n"
     "        if (a[i] > best_value) { best_value = a[i]; best = i; }\n"
     "    }\n"},
    {"extremum-shape", DECISION_VECTOR_REFUSAL,
     "Running minimum or maximum the kernel just missed",
     "`if (a[i] > best) { best = a[i]; }` is a shape the compiler vectorizes:\n"
     "it reads the diamond as the operator it is and emits vmaxps or vmaxpd,\n"
     "or vpmaxsd for int32, seeding every lane with the accumulator's incoming\n"
     "value. Something other than the branch stopped this loop:\n"
     "\n"
     "  - The counter does not start at 0. A scan seeded from `a[0]` and run\n"
     "    from `i = 1` is outside the kernel, which treats the loop's bound as\n"
     "    an element count from the base. Start at 0 and seed the accumulator\n"
     "    with a sentinel instead.\n"
     "\n"
     "  - The elements are not float32, float64 or int32. Those are the lane\n"
     "    widths the extremum kernel carries, and the width it reads is the\n"
     "    element's. A uint8 or int16 array stays scalar however the\n"
     "    accumulator is declared, so widening `best` alone moves nothing;\n"
     "    widen the array. uint32 is refused because vpmaxsd compares signed.\n"
     "\n"
     "  - The elements are narrower than the accumulator. `(int32)bytes[i]`\n"
     "    widens per element; the kernel's lanes are the element width. The\n"
     "    compiler has no kernel for that yet.\n"
     "\n"
     "  - The body also stores. Then it is a clamp, which\n"
     "    `mettle explain clamp-store` covers.\n"},
    {"predicated-count", DECISION_VECTOR_REFUSAL,
     "An accumulator updated only on the taken arm",
     "Over int32 elements, `if (a[i] > t) { c = c + 1; }` and\n"
     "`if (a[i] < 0) { s = s + a[i]; }` both vectorize: the comparison already\n"
     "holds 0 or 1, so the compiler multiplies the addend by it and the body\n"
     "becomes straight line.\n"
     "\n"
     "This code fires when that rewrite does not apply. Three shapes it will\n"
     "not take:\n"
     "\n"
     "  - Float elements. There is no predicated float reduction, and the\n"
     "    branchless form has no kernel either, so there is nothing to change\n"
     "    in the loop.\n"
     "  - An else arm that also writes the accumulator. That is a select on a\n"
     "    loop-carried value, with no single addend to accumulate.\n"
     "  - A guard on a computed value. The conversion reads a comparison of a\n"
     "    loaded element (`a[i] > t`, `a[i] == 0`); `if (a[i] & 6)` is a test\n"
     "    of an expression, and respelling it `(a[i] & 6) != 0` keeps it one.\n"
     "    Add the comparison to the accumulator and the kernel takes it, for\n"
     "    any comparison:\n"
     "\n"
     "        c = c + ((a[i] & 6) != 0);\n"},
    {"clamp-store", DECISION_VECTOR_REFUSAL,
     "A value clamped or selected before it is stored",
     "`if (v > hi) { v = hi; }` before `a[i] = v` is a clamp, and over int32\n"
     "elements it vectorizes: the branch becomes vpminsd or vpmaxsd, or a\n"
     "lane select when the arms are not the two compared values. A chain of\n"
     "them, including the one an inlined helper's early returns leave behind,\n"
     "folds the same way.\n"
     "\n"
     "This code fires when that conversion did not carry the loop. Two things\n"
     "stop it: float32 and float64 elements, which have no select kernel yet,\n"
     "and a nest deep enough that the values live at once outnumber the ymm\n"
     "registers the kernel has, which is six for an int32 map. Three levels of\n"
     "`if` and `else` over distinct expressions is past that.\n"
     "\n"
     "Splitting the deepest arm into its own loop is what usually helps.\n"},
    {"strided-access", DECISION_VECTOR_REFUSAL,
     "The loop steps more than one element at a time",
     "`rgb[i * 3 + 1]`, `dst[i * 2]`, and any other non-unit stride. Every\n"
     "kernel walks its arrays one contiguous vector per iteration, so a\n"
     "strided access has no kernel to land in. There is no gather or\n"
     "scatter form.\n"
     "\n"
     "The stride usually comes from the data layout, so there is often\n"
     "nothing to change. When the layout is free to change, splitting an\n"
     "interleaved array into one array per component makes every loop over it\n"
     "unit-stride: `rgb[i*3+0]`, `rgb[i*3+1]` and `rgb[i*3+2]` become `r[i]`,\n"
     "`g[i]` and `b[i]`, three loops that all vectorize.\n"},
    {"unbounded-shift", DECISION_VECTOR_REFUSAL,
     "A right shift whose input cannot be bounded",
     "Integer lanes are 32 bits wide. `+ - * & | ^ <<` are congruent mod 2^32,\n"
     "so the low 32 bits of a result depend only on the low 32 bits of its\n"
     "inputs and a lane reproduces them whatever width the scalar code used.\n"
     "`>>` is the exception: it moves high bits down, so a lane that wrapped\n"
     "where the scalar did not would shift different bits in.\n"
     "\n"
     "The kernel takes a right shift only where the shifted value is provably\n"
     "inside int32, and a multiply is not provable on its own. This is refused:\n"
     "\n"
     "    dst[i] = (r[i]*77 + g[i]*150 + b[i]*29) >> 8;\n"
     "\n"
     "Masking bounds it, at the cost of one op per lane, and this vectorizes:\n"
     "\n"
     "    dst[i] = ((r[i]*77 + g[i]*150 + b[i]*29) & 65535) >> 8;\n"
     "\n"
     "Keep the destination int32. Narrowing the store to a `uint8*` puts the\n"
     "loop outside the kernels for a separate reason, so masking alone will\n"
     "not move it.\n"},
    {"variable-shift", DECISION_VECTOR_REFUSAL,
     "A shift by a distance read at run time",
     "The kernels carry a shift only when the distance is written in the\n"
     "source. A distance the loop reads has no kernel, in either direction:\n"
     "\n"
     "    b[i] = a[i] >> k;      // `k` a parameter or a variable\n"
     "    b[i] = a[i] << k;\n"
     "\n"
     "Both stay scalar. A constant distance vectorizes:\n"
     "\n"
     "    b[i] = a[i] >> 8;\n"
     "\n"
     "There is no source rewrite that reaches a kernel while the distance\n"
     "stays a run-time value, so this is a gap in the compiler rather than a\n"
     "problem with the loop. Splitting into one loop per distance works when\n"
     "the distances are few and known.\n"},
    {"unrecognized-shape", DECISION_VECTOR_REFUSAL,
     "No recognizer claimed this loop",
     "No kernel matched the loop, and the lane vectorizer, which takes most\n"
     "counted loops, stopped on the loop itself: its test is not a signed\n"
     "`counter < bound`, or its counter does not step by one as the last\n"
     "statement. The remark names which. When the lane vectorizer left no\n"
     "verdict and the report finds no call, branch, recurrence, carried\n"
     "value or element width to blame, the remark says so.\n"
     "\n"
     "What does vectorize, as a checklist:\n"
     "  - unit-stride accesses, `a[i]` and not `a[i * k]`\n"
     "  - int8, int32, float32 or float64 elements\n"
     "  - a straight-line body: no calls, no branches, no allocation\n"
     "  - one of the known shapes: a map (`a[i] = expr`), a `+` reduction\n"
     "    (`s = s + expr`), a dot product, or a fill\n"
     "\n"
     "A loop that meets all of that and still lands here is worth reporting.\n"},

    {"callee-no-body", DECISION_INLINE_REFUSAL,
     "The callee has no body to inline",
     "The function is declared but not defined in this program: an `extern`,\n"
     "or a runtime entry point. There is nothing to copy into the caller.\n"
     "\n"
     "This is not a problem. A call to a C function or an OS API is a call.\n"},
    {"callee-noinline", DECISION_INLINE_REFUSAL,
     "The callee is marked @noinline",
     "An absolute veto. @noinline is not a hint, and no budget or heuristic\n"
     "overrides it.\n"
     "\n"
     "Fix: remove @noinline where inlining is what you want. Where it is on\n"
     "the function deliberately (to keep a call boundary for profiling, or to\n"
     "shape code layout), this remark is the compiler agreeing with you.\n"
     "\n"
     "The report simulates removing it and tells you what inlining would have\n"
     "unlocked, so the cost of the decorator is visible rather than assumed.\n"},
    {"callee-denylisted", DECISION_INLINE_REFUSAL,
     "The callee is on the compiler's inline denylist",
     "A small set of names is refused by hand, as a guard against compile-time\n"
     "blowup in shapes known to explode.\n"
     "\n"
     "Fix: mark the callee @inline to override the denylist. Watch compile\n"
     "time afterwards, since that is the cost the guard exists to prevent.\n"},
    {"too-many-parameters", DECISION_INLINE_REFUSAL,
     "The callee takes more than 16 parameters",
     "Past sixteen parameters the inliner's argument-binding work grows faster\n"
     "than the inlining is worth.\n"
     "\n"
     "Fix: pass a struct. That is usually the better interface anyway, and it\n"
     "brings the function back under the cap.\n"},
    {"callee-parameter-names", DECISION_INLINE_REFUSAL,
     "The callee's parameter names are unavailable",
     "The inliner binds arguments to parameters by name, and this callee\n"
     "reached the optimizer without them.\n"
     "\n"
     "A compiler limitation rather than anything in your code.\n"},
    {"callee-over-budget", DECISION_INLINE_REFUSAL,
     "The callee's body is over the inline size budget",
     "Inlining copies the body into every call site, so a large callee called\n"
     "in ten places makes the program larger and the instruction cache colder.\n"
     "The budget is the line the compiler draws, and --pgo moves that line for\n"
     "callees measured hot.\n"
     "\n"
     "Fixes:\n"
     "  - mark the callee @inline to override the budget\n"
     "  - compile with --pgo, so a measured-hot callee earns the larger budget\n"
     "    on evidence rather than on a decorator\n"
     "\n"
     "Before printing the @inline advice the report applies it to a scratch\n"
     "copy and re-runs the check, so it never suggests a decorator that a\n"
     "structural guard behind the budget would refuse anyway.\n"},
    {"callee-call-count", DECISION_INLINE_REFUSAL,
     "The callee makes more calls than the inline call-count budget allows",
     "The callee's body is small enough, but it is mostly calls to other\n"
     "functions: a glue routine that orchestrates helpers. Inlining one of\n"
     "those copies the orchestration into the caller and gains nothing, since\n"
     "the calls it makes are still calls.\n"
     "\n"
     "Fix: mark the callee @inline to override the cap, where the caller\n"
     "really does benefit from seeing through the glue.\n"},
    {"callee-inline-asm", DECISION_INLINE_REFUSAL,
     "The callee contains inline assembly",
     "An asm block can depend on the frame it was written for. Copying it into\n"
     "another function is not safe in general, so the inliner does not.\n"
     "\n"
     "A structural guard: @inline does not override it.\n"},
    {"callee-has-loop", DECISION_INLINE_REFUSAL,
     "The callee has a loop and is over the size budget for one",
     "A small callee with a loop inlines, because that is what exposes its\n"
     "loop to values the caller already holds in registers. The budget\n"
     "counts the whole callee, 80 instructions, and doubles at sites two or\n"
     "more loops deep. Past it the call is noise next to the loop it\n"
     "reaches, and the caller growth is not repaid.\n"
     "\n"
     "@inline inlines it anyway. Where the loop is hot, look at its own\n"
     "remark first: that is usually where the time goes.\n"},
    {"callee-no-return",  DECISION_INLINE_REFUSAL,
     "The callee has no return instruction to rewrite",
     "Inlining works by rewriting the callee's returns into assignments in the\n"
     "caller. A body with no return has nothing to rewrite. A function that\n"
     "only ever traps or loops forever looks like this.\n"
     "\n"
     "A structural guard: @inline does not override it.\n"},
    {"callee-has-kernel", DECISION_INLINE_REFUSAL,
     "The callee was kept as a call and its loop became a SIMD kernel",
     "The inliner refused this call for the reason the remark gives first.\n"
     "Vectorization runs after inlining, and it later turned the callee's\n"
     "loop into a kernel.\n"
     "\n"
     "The kernel runs at the same speed either way. One call to reach it is\n"
     "not measurable against the loop it contains, so the refusal costs\n"
     "nothing here.\n"},
    {"recursive", DECISION_INLINE_REFUSAL,
     "The call is directly recursive",
     "A function calling itself cannot be inlined without bound. Mettle\n"
     "expands bounded self-recursion automatically where it can prove the\n"
     "depth; this remark is what is left over.\n"
     "\n"
     "Fix: rewrite as a loop where the recursion is hot and the depth is not\n"
     "provable. A loop also gives the vectorizer something to work with.\n"},
    {"caller-over-budget", DECISION_INLINE_REFUSAL,
     "The calling function is over its budget and this site is cold",
     "The caller has grown past the point where more inlining pays, and this\n"
     "particular call site is neither inside a loop nor measured hot. It runs\n"
     "at most once per call of the caller, so leaving it a real call costs\n"
     "nothing you can measure.\n"
     "\n"
     "Sites exempt from this rule, and so still inlined: calls inside loops,\n"
     "sites measured hot under --pgo, tiny call-free callees, and callees\n"
     "marked @inline.\n"
     "\n"
     "There is deliberately no fix advice. For a cold one-shot site, not\n"
     "inlining is the right answer.\n"},
    {"argument-count", DECISION_INLINE_REFUSAL,
     "The call's argument count does not match the callee",
     "The site passes a different number of arguments than the callee declares,\n"
     "or more than the inliner handles. A variadic call looks like this.\n"
     "\n"
     "A structural guard: @inline does not override it.\n"},
    {"rounds-exhausted", DECISION_INLINE_REFUSAL,
     "Inlining rounds ran out before this call was revisited",
     "The inliner runs four rounds over the program. This call site appeared\n"
     "in the last of them, copied in with the body of another callee, and\n"
     "the inliner stopped before it came back to look at it.\n"
     "\n"
     "Deep call chains hit this: each level of nesting costs a round. No\n"
     "decorator changes the round count. Flattening the chain by one level,\n"
     "or calling the inner function directly, brings the site into reach.\n"},
    {"call-after-inlining", DECISION_INLINE_REFUSAL,
     "A pass after the inliner created this call",
     "The inliner never saw this call site. A pass that runs after it, such\n"
     "as one that moves a call out of a loop or turns a loop into a call to\n"
     "a helper, put the call where it is now.\n"
     "\n"
     "Nothing in the source decides this, so there is no fix to apply.\n"},

    {"vectorized", DECISION_APPLIED,
     "The loop became a SIMD kernel",
     "The whole loop now runs as vector instructions, several elements per\n"
     "step. The remark names the kernel and the width, for example\n"
     "`vfmadd231ps, 8-wide float32`, so you can check that the width matches\n"
     "the element type you meant to use.\n"},
    {"vectorized-inner", DECISION_APPLIED,
     "The inner loop of a nest became a kernel",
     "The inner loop vectorized and the outer loop stays scalar, driving it.\n"
     "This is the normal outcome for a nest, and usually the one you want: the\n"
     "inner loop is where the elements are.\n"},
    {"outer-of-nest", DECISION_APPLIED,
     "The outer loop of a nest, reported for context",
     "Recorded so the report accounts for every loop rather than going quiet\n"
     "about the outer one. Only innermost loops vectorize. The verdict that\n"
     "matters is on the inner loop's own remark.\n"},
    {"eliminated", DECISION_APPLIED,
     "The loop was removed",
     "The optimizer proved the loop had no effect anyone could observe, or\n"
     "folded it to a constant, so it emits nothing at all.\n"
     "\n"
     "Worth a glance in a benchmark. A loop that was meant to measure\n"
     "something and got eliminated measures nothing.\n"},
    {"rewrite-applied", DECISION_APPLIED,
     "An expression was rewritten by a `rewrite` rule",
     "The expression had the shape of a rule's `from` side, the rule's\n"
     "`where` guard (if any) held on the constants at this site, and the\n"
     "expression became the rule's `to` side. The function was then executed\n"
     "before and after the change on generated inputs, and only a match in\n"
     "every observation lets the rewrite stand.\n"},
    {"rewrite-rule-checked", DECISION_APPLIED,
     "A `rewrite` rule's two sides were shown to agree",
     "Before any rule is applied, its `from` and `to` sides are executed in\n"
     "the compile-time interpreter on generated inputs: the fixed table the\n"
     "translation validator uses, the constants the rule itself compares\n"
     "against, a set of awkward integers (negative, one past each power of\n"
     "two, the extremes of every width), and, for rules with two or three\n"
     "integer parameters, every combination of those. A rule with a `where`\n"
     "clause is checked on the inputs the clause admits, and the remark says\n"
     "how many that was. Disagreement on any input is a compile error with\n"
     "the input printed.\n"},
    {"rewrite-rule-applied", DECISION_APPLIED,
     "How many expressions a `rewrite` rule rewrote",
     "The count over the whole program, so a rule written for a hot loop can\n"
     "be seen to have reached it.\n"},
    {"rewrite-rule-unused", DECISION_VECTOR_REFUSAL,
     "A `rewrite` rule matched nothing",
     "No expression had the shape of the rule's `from` side at any point the\n"
     "compiler looked: before inlining, when calls are still calls, and after\n"
     "it, when arithmetic has been folded and propagated. The rule costs\n"
     "nothing, but it is doing nothing.\n"
     "\n"
     "The usual causes: the rule's parameter types differ from the code's (a\n"
     "rule over int64 does not match int32 arithmetic), or the compiler's own\n"
     "folding reached the expression first.\n"},
    {"rewrite-rule-guard-undecided", DECISION_VECTOR_REFUSAL,
     "A `rewrite` rule matched, but its guard needed a runtime value",
     "A `where` clause is evaluated at compile time, on the constants bound at\n"
     "the match site. Every match bound a parameter the guard reads to a\n"
     "value only known at run time, so the guard could not be decided and the\n"
     "rule did not apply.\n"
     "\n"
     "Fix: if the rule holds for every value, drop the guard. If it does not,\n"
     "the guard is doing its job, and only sites with constant arguments\n"
     "qualify.\n"},
    {"rewrite-rule-guard-false", DECISION_VECTOR_REFUSAL,
     "A `rewrite` rule matched, but its guard was false",
     "Every match bound constants on which the `where` clause evaluated to\n"
     "false. The rule declined correctly; nothing to change.\n"},
    {"inlined", DECISION_APPLIED,
     "The call was inlined",
     "The callee's body was copied into the caller and the call is gone. This\n"
     "is also what makes the surrounding loop vectorizable, since a call in\n"
     "the body would have blocked it.\n"},
    {"unrolled", DECISION_APPLIED,
     "The loop was unrolled",
     "The body was replicated so fewer iterations do more work each, which\n"
     "removes counter arithmetic and branch overhead. A loop with a constant\n"
     "trip count small enough is unrolled fully, and then no loop is left.\n"},
    {"hoisted", DECISION_APPLIED,
     "A call was lifted out of the loop",
     "The callee is @pure and its arguments do not change across iterations,\n"
     "so the result cannot change either. It is computed once before the loop.\n"
     "\n"
     "This is what @pure buys. Without it the compiler must assume the call\n"
     "could do anything, and must repeat it.\n"},
    {"if-converted", DECISION_APPLIED,
     "A branch became branchless arithmetic",
     "A short `if` in the body was rewritten as a select or a mask, so both\n"
     "sides are computed and one is chosen. That removes a mispredictable\n"
     "branch, and it is what lets a loop with a small conditional vectorize.\n"},
    {"prefetched", DECISION_APPLIED,
     "A prefetch was inserted",
     "The loop walks memory with a stride the compiler can see ahead of, so it\n"
     "asks for the line some iterations early. This hides part of the memory\n"
     "latency on a loop whose limit is bandwidth rather than arithmetic.\n"},
    {"noalloc-verified", DECISION_APPLIED,
     "The @noalloc contract holds",
     "The function is marked @noalloc, and the compiler walked everything it\n"
     "can reach and found no allocation on any path. The contract is proved;\n"
     "a violation would have stopped the build with an error.\n"},
    {"local-memory", DECISION_DEVICE,
     "A kernel's local was emitted as memory",
     "A scalar local of a kernel lives in a register. An array or a record\n"
     "lives in registers too when every use names one element with a constant\n"
     "index, because each element can then take a register of its own. A\n"
     "use that indexes with a run-time value, passes the address to a helper\n"
     "or stores the address keeps the whole value in the work item's own\n"
     "memory, and every access to it becomes a load or a store.\n"
     "\n"
     "On PTX that memory is `.local`. Where ptxas is on PATH the report reads\n"
     "its stack frame for the kernel: a frame at least as large as the value\n"
     "confirms it, and a frame of zero means the assembler kept the value in\n"
     "registers, so the report counts it as a register value and lists\n"
     "nothing for it.\n"
     "\n"
     "On Metal the value stays one array or record in the emitted source,\n"
     "and Apple's compiler decides where it lives.\n"
     "\n"
     "Fix: index it with constants, which `comptime for` gives a loop with a\n"
     "constant count, or keep the running values in scalar locals.\n"},
    {"generic-access", DECISION_DEVICE,
     "A load or store goes through the generic address space",
     "A device pointer whose type names its space loads with `ld.global`,\n"
     "`ld.shared` or `ld.local`. A plain `T*` that can hold a workgroup or a\n"
     "private address is a generic pointer: the address is converted where\n"
     "it becomes one, and every access through it resolves the space at run\n"
     "time. A kernel's own plain pointer parameters only ever hold global\n"
     "addresses, so they still load `.global`; this remark is about helpers\n"
     "called with more than one kind of address.\n"
     "\n"
     "Metal has no generic space. The same helper is compiled once for each\n"
     "space it is called with, and the report says how many copies it made.\n"
     "\n"
     "Fix: name the space in the helper's parameter type, `T global*`,\n"
     "`T shared*` or `T local*`.\n"},
    {"vector-load", DECISION_DEVICE,
     "Adjacent loads became one vector load",
     "Two or four loads at consecutive offsets from one pointer became a\n"
     "single `ld.v2` or `ld.v4`. The pointer's type declares the alignment\n"
     "the wider load needs, which is what licenses it.\n"},
    {"vector-load-alignment", DECISION_DEVICE,
     "Adjacent loads stayed separate for want of a declared alignment",
     "The kernel loads two or four adjacent elements from one pointer, which\n"
     "is the shape of a vector load. A vector load of N elements needs the\n"
     "address aligned to N elements, and the pointer's type declares less\n"
     "than that or nothing, so each element is loaded on its own.\n"
     "\n"
     "Fix: declare the alignment in the pointer's type, such as\n"
     "`float32 global align(16)*`. On a kernel parameter the launch is then\n"
     "held to it. On a computed address the cast to the aligned type is\n"
     "accepted where the compiler can prove the offset is a multiple of the\n"
     "alignment, which a declared type on the offset gives it.\n"},
    {"tensor-instruction", DECISION_DEVICE,
     "The instruction a tensor operation became",
     "A `tensor_mma` or `tensor_matmul` names a product of tiles, and each\n"
     "target has its own way to run one. On PTX it is `mma.sync` where the\n"
     "element types have a native form, or `wmma.mma`; a logical tile larger\n"
     "than the hardware tile runs as several hardware tiles a step. On Metal\n"
     "it is `simdgroup_matrix` 8 by 8 tiles for f16, bf16 and f32 inputs.\n"
     "The report names the form, the shape and the element type that were\n"
     "chosen.\n"},
    {"tensor-resident", DECISION_DEVICE,
     "A tensor accumulator stays in registers across its steps",
     "Steps that accumulate into the same output would each load the\n"
     "accumulator from memory and store it back. Where nothing between the\n"
     "steps can observe the output, it is loaded once, kept in the tensor\n"
     "registers across the chain, the loop or the region, and stored once at\n"
     "the end. The report gives the registers that takes against the budget.\n"},
    {"tensor-register-budget", DECISION_DEVICE,
     "A tensor accumulator went back to memory because of the register budget",
     "Keeping an accumulator in registers across its steps takes the\n"
     "accumulator's registers plus one step's operands. That count is over\n"
     "the tensor register budget, so every step loads the accumulator and\n"
     "stores it again.\n"
     "\n"
     "Fix: raise the budget with `--gpu-tensor-tuple-budget=N`. The report\n"
     "names the N that fits. A larger budget leaves fewer registers for the\n"
     "rest of the kernel, and the occupancy row says what that costs.\n"},
    {"tensor-scalar-replay", DECISION_DEVICE,
     "A tensor_matmul runs one element at a time",
     "No tensor instruction on this target fits the operation, or the one\n"
     "that fits needs more registers than the budget allows. The whole warp\n"
     "then computes the product element by element in the order the\n"
     "numerics contract fixes, so the result is the same and the speed is\n"
     "that of scalar code.\n"
     "\n"
     "Fix: where the reason is the budget, raise it with\n"
     "`--gpu-tensor-tuple-budget=N`. Otherwise pick element types and a shape\n"
     "the target has an instruction for.\n"},
    {"tensor-scalar", DECISION_DEVICE,
     "A block-scaled integer tensor product runs in scalar code on Metal",
     "Apple GPUs have no integer matrix unit. A block-scaled int8 product is\n"
     "computed as exact integer dot products in scalar code, in the rounding\n"
     "sequence the CPU grid runner uses, so it gives the bits the PTX path\n"
     "gives.\n"},
    {"tensor-observable", DECISION_DEVICE,
     "Something between two tensor steps could observe the accumulator",
     "Two steps accumulate into the same output, and an operation sits\n"
     "between them that could read or write that output: a call, an atomic,\n"
     "a load, a store or a barrier. The output therefore has to be in memory\n"
     "at that point, so the step before stores it and the step after loads\n"
     "it again.\n"
     "\n"
     "Fix: move the operation before the first step or after the last, or\n"
     "out of the loop. The report applies that to a copy of the kernel and\n"
     "prints the fix only when the accumulator then stays in registers.\n"},
    {"staging-barrier-order", DECISION_DEVICE,
     "Copies into workgroup memory were not staged asynchronously",
     "A run of copies from global to workgroup memory followed by a\n"
     "workgroup barrier is the shape asynchronous staging takes: the copies\n"
     "are issued, and the barrier waits for them. That is only valid when\n"
     "the barrier publishes what was copied, which takes one that both\n"
     "acquires and releases. This one does less, so the copies stay\n"
     "ordinary loads and stores.\n"
     "\n"
     "Fix: write `barrier(workgroup, acq_rel)`.\n"},
    {"staging-interrupted", DECISION_DEVICE,
     "An operation sits between the staged copies and their barrier",
     "Copies from global to workgroup memory can be issued asynchronously\n"
     "when nothing but register arithmetic runs between them and the\n"
     "barrier that publishes them. A load, a store or a call in that\n"
     "stretch could see workgroup memory before the copies land.\n"
     "\n"
     "Fix: move it after the barrier.\n"},
    {"async-copy", DECISION_DEVICE,
     "What a staged copy became",
     "On PTX for sm_80 and later at PTX 7.0 a staged copy is `cp.async`, and\n"
     "the barrier that follows waits for it. On an older target it is an\n"
     "ordinary load and store. Metal has no asynchronous copy, so there it\n"
     "is an assignment, in order.\n"},
    {"register-tiles", DECISION_DEVICE,
     "The registers a kernel's tiles hold",
     "A local whose type is a fragment layout is a register tile: one work\n"
     "item's share of an MMA tile, held in registers. The report lists each\n"
     "tile with the registers it takes and the lines it is live across, and\n"
     "the peak for the kernel against the budget of 255 a work item. An\n"
     "intermediate tile used once is computed one element at a time and\n"
     "holds none.\n"},
    {"atomic-fence", DECISION_DEVICE,
     "An ordered atomic became a relaxed one beside a fence",
     "Metal 3.2 atomics take no memory order. An ordered load, store or\n"
     "exchange is emitted as the relaxed form with `atomic_thread_fence`\n"
     "before it, after it, or both, which gives the order asked for. Metal\n"
     "4.1 has the orders natively, and `--metal-version=4.1` emits them.\n"},
    {"structured-exit", DECISION_DEVICE,
     "A kernel's control flow was rebuilt with an exit variable",
     "Metal has no `goto`, so the compiler rebuilds `if`, `while` and\n"
     "`switch` from the kernel's control flow. A branch that leaves more\n"
     "than one enclosing loop or block at once has no structured form, so it\n"
     "sets a variable and breaks, and each enclosing construct tests the\n"
     "variable on the way out.\n"},
    {"helper-specialized", DECISION_DEVICE,
     "A helper was compiled once for each address space",
     "Metal has no generic address space. A helper that takes a plain `T*`\n"
     "and is called with a workgroup address at one site and a device\n"
     "address at another is emitted twice, once for each.\n"},
    {"float64-narrowed", DECISION_DEVICE,
     "A float64 computation runs in float32 on Metal",
     "Apple GPUs have no double. Where every operand is a float32 value or\n"
     "a literal that float32 holds exactly, and the result is only rounded\n"
     "back to float32, the computation gives the same bits in float32, and\n"
     "that is what is emitted. Anything else in float64 is refused.\n"},
    {"dispatch", DECISION_DEVICE,
     "A kernel launch",
     "A `dispatch` statement launches a kernel. The report gives what the\n"
     "compiler could fold at the site: the work items and the blocks they\n"
     "make where the count is a constant, or the block size alone where the\n"
     "count is read at run time.\n"},
};

#define DOCS_COUNT (sizeof(DOCS) / sizeof(DOCS[0]))
#define DECISIONS_COUNT (sizeof(DECISIONS) / sizeof(DECISIONS[0]))

static const char *decision_group_title(DecisionGroup group) {
  switch (group) {
  case DECISION_VECTOR_REFUSAL:
    return "Why a loop did not vectorize";
  case DECISION_INLINE_REFUSAL:
    return "Why a call was not inlined";
  case DECISION_APPLIED:
    return "What the optimizer applied";
  case DECISION_DEVICE:
    return "What a kernel became on the GPU";
  }
  return "Optimizer decisions";
}

static void explain_json_string(const char *s) {
  putchar('"');
  for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
    switch (*p) {
    case '"':
      fputs("\\\"", stdout);
      break;
    case '\\':
      fputs("\\\\", stdout);
      break;
    case '\n':
      fputs("\\n", stdout);
      break;
    case '\r':
      fputs("\\r", stdout);
      break;
    case '\t':
      fputs("\\t", stdout);
      break;
    default:
      if (*p < 0x20) {
        printf("\\u%04x", *p);
      } else {
        putchar((int)*p);
      }
    }
  }
  putchar('"');
}

static const char *decision_group_slug(DecisionGroup group) {
  switch (group) {
  case DECISION_VECTOR_REFUSAL:
    return "vectorization-refusal";
  case DECISION_INLINE_REFUSAL:
    return "inline-refusal";
  case DECISION_APPLIED:
    return "applied";
  case DECISION_DEVICE:
    return "device";
  }
  return "other";
}

static void print_code_json(void) {
  printf("{\"schema\":1,\"codes\":[");
  for (size_t i = 0; i < DOCS_COUNT; i++) {
    printf("%s{\"code\":", i ? "," : "");
    explain_json_string(DOCS[i].code);
    printf(",\"kind\":\"diagnostic\",\"group\":\"diagnostic\",\"groupTitle\":");
    explain_json_string("Compile diagnostics");
    printf(",\"title\":");
    explain_json_string(DOCS[i].title);
    printf(",\"body\":");
    explain_json_string(DOCS[i].body);
    printf("}");
  }
  for (size_t i = 0; i < DECISIONS_COUNT; i++) {
    printf(",{\"code\":");
    explain_json_string(DECISIONS[i].code);
    printf(",\"kind\":\"decision\",\"group\":");
    explain_json_string(decision_group_slug(DECISIONS[i].group));
    printf(",\"groupTitle\":");
    explain_json_string(decision_group_title(DECISIONS[i].group));
    printf(",\"title\":");
    explain_json_string(DECISIONS[i].title);
    printf(",\"body\":");
    explain_json_string(DECISIONS[i].body);
    printf("}");
  }
  printf("]}\n");
}

static void print_code_list(void) {
  printf("Diagnostic codes (compile errors and warnings):\n");
  for (size_t i = 0; i < DOCS_COUNT; i++) {
    printf("  %-22s %s\n", DOCS[i].code, DOCS[i].title);
  }

  const DecisionGroup groups[] = {DECISION_VECTOR_REFUSAL,
                                  DECISION_INLINE_REFUSAL, DECISION_APPLIED,
                                  DECISION_DEVICE};
  for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); g++) {
    printf("\n%s (--explain decision codes):\n",
           decision_group_title(groups[g]));
    for (size_t i = 0; i < DECISIONS_COUNT; i++) {
      if (DECISIONS[i].group == groups[g]) {
        printf("  %-22s %s\n", DECISIONS[i].code, DECISIONS[i].title);
      }
    }
  }

  printf("\nRun `mettle explain <CODE>` for details on any of them, for "
         "example:\n");
  printf("  mettle explain E0004\n");
  printf("  mettle explain dot-shape-address\n");
}

static void normalize_code(const char *code, char *out, size_t cap) {
  size_t n = 0;
  for (const char *p = code; *p && n + 1 < cap; p++) {
    if (*p == '[' || *p == ']' || *p == '`' || *p == '\'' || *p == '"' ||
        *p == ',' || *p == '.') {
      continue;
    }
    out[n++] = *p;
  }
  out[n] = '\0';
}

static int code_equal_fold(const char *a, const char *b) {
  for (; *a && *b; a++, b++) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') {
      ca = (char)(ca - 'A' + 'a');
    }
    if (cb >= 'A' && cb <= 'Z') {
      cb = (char)(cb - 'A' + 'a');
    }
    if (ca == '_') {
      ca = '-';
    }
    if (cb == '_') {
      cb = '-';
    }
    if (ca != cb) {
      return 0;
    }
  }
  return *a == '\0' && *b == '\0';
}

static int code_contains_fold(const char *code, const char *fragment) {
  size_t n = strlen(fragment);
  size_t len = strlen(code);
  if (n == 0 || n > len) {
    return 0;
  }
  for (size_t start = 0; start + n <= len; start++) {
    size_t k = 0;
    for (; k < n; k++) {
      char a = code[start + k], b = fragment[k];
      if (a >= 'A' && a <= 'Z') {
        a = (char)(a - 'A' + 'a');
      }
      if (b >= 'A' && b <= 'Z') {
        b = (char)(b - 'A' + 'a');
      }
      if (a == '_') {
        a = '-';
      }
      if (b == '_') {
        b = '-';
      }
      if (a != b) {
        break;
      }
    }
    if (k == n) {
      return 1;
    }
  }
  return 0;
}

static size_t edit_distance(const char *a, const char *b, size_t limit) {
  size_t la = strlen(a), lb = strlen(b);
  if (la > lb ? la - lb > limit : lb - la > limit) {
    return limit + 1;
  }
  size_t row[128];
  if (lb + 1 > sizeof(row) / sizeof(row[0])) {
    return limit + 1;
  }
  for (size_t j = 0; j <= lb; j++) {
    row[j] = j;
  }
  for (size_t i = 1; i <= la; i++) {
    size_t diagonal = row[0];
    row[0] = i;
    for (size_t j = 1; j <= lb; j++) {
      size_t previous = row[j];
      char ca = a[i - 1], cb = b[j - 1];
      if (ca >= 'A' && ca <= 'Z') {
        ca = (char)(ca - 'A' + 'a');
      }
      if (cb >= 'A' && cb <= 'Z') {
        cb = (char)(cb - 'A' + 'a');
      }
      size_t substitute = diagonal + (ca == cb ? 0 : 1);
      size_t insert = row[j - 1] + 1;
      size_t remove = row[j] + 1;
      size_t best = substitute < insert ? substitute : insert;
      row[j] = best < remove ? best : remove;
      diagonal = previous;
    }
  }
  return row[lb];
}

static const char *nearest_code(const char *code) {
  const char *best = NULL;
  size_t best_distance = 4;

  for (size_t i = 0; i < DECISIONS_COUNT; i++) {
    size_t d = edit_distance(code, DECISIONS[i].code, best_distance);
    if (d < best_distance) {
      best_distance = d;
      best = DECISIONS[i].code;
    }
  }
  for (size_t i = 0; i < DOCS_COUNT; i++) {
    size_t d = edit_distance(code, DOCS[i].code, best_distance);
    if (d < best_distance) {
      best_distance = d;
      best = DOCS[i].code;
    }
  }
  return best;
}

int mettle_explain_lookup(const char *code, const char **title,
                          const char **body) {
  char normalized[128];
  if (!code || !code[0]) {
    return 0;
  }
  normalize_code(code, normalized, sizeof(normalized));
  for (size_t i = 0; i < DOCS_COUNT; i++) {
    if (code_equal_fold(DOCS[i].code, normalized)) {
      *title = DOCS[i].title;
      *body = DOCS[i].body;
      return 1;
    }
  }
  for (size_t i = 0; i < DECISIONS_COUNT; i++) {
    if (code_equal_fold(DECISIONS[i].code, normalized)) {
      *title = DECISIONS[i].title;
      *body = DECISIONS[i].body;
      return 1;
    }
  }
  return 0;
}

int mettle_explain_error_code(const char *code) {
  if (!code || strcmp(code, "list") == 0 || strcmp(code, "all") == 0) {
    print_code_list();
    return 0;
  }
  if (strcmp(code, "--json") == 0) {
    print_code_json();
    return 0;
  }

  char normalized[128];
  normalize_code(code, normalized, sizeof(normalized));

  for (size_t i = 0; i < DOCS_COUNT; i++) {
    if (code_equal_fold(DOCS[i].code, normalized)) {
      printf("%s: %s\n\n%s", DOCS[i].code, DOCS[i].title, DOCS[i].body);
      return 0;
    }
  }

  for (size_t i = 0; i < DECISIONS_COUNT; i++) {
    if (code_equal_fold(DECISIONS[i].code, normalized)) {
      printf("%s: %s\n", DECISIONS[i].code, DECISIONS[i].title);
      printf("(%s, reported by --explain)\n\n%s",
             decision_group_title(DECISIONS[i].group), DECISIONS[i].body);
      return 0;
    }
  }

  if (strlen(normalized) >= 3) {
    const DecisionDoc *only = NULL;
    size_t hits = 0;
    for (size_t i = 0; i < DECISIONS_COUNT; i++) {
      if (code_contains_fold(DECISIONS[i].code, normalized)) {
        only = &DECISIONS[i];
        hits++;
      }
    }
    if (hits == 1) {
      printf("%s: %s\n", only->code, only->title);
      printf("(%s, reported by --explain)\n\n%s",
             decision_group_title(only->group), only->body);
      return 0;
    }
    if (hits > 1) {
      printf("'%s' matches %zu codes:\n", code, hits);
      for (size_t i = 0; i < DECISIONS_COUNT; i++) {
        if (code_contains_fold(DECISIONS[i].code, normalized)) {
          printf("  %-22s %s\n", DECISIONS[i].code, DECISIONS[i].title);
        }
      }
      return 0;
    }
  }

  fprintf(stderr, "error: unknown code '%s'\n", code);
  if (normalized[0] == 'R' && normalized[1] >= '1' && normalized[1] <= '9') {
    fprintf(stderr,
            "help: a code above R0999 belongs to a rule the program wrote, "
            "and the text lives on the declaration: run `mettle explain %s "
            "<file.mettle>`\n",
            normalized);
    return 1;
  }
  const char *near = nearest_code(normalized);
  if (near) {
    fprintf(stderr, "help: did you mean `%s`? Run `mettle explain %s`\n", near,
            near);
  } else {
    fprintf(stderr, "help: run `mettle explain list` for the full index\n");
  }
  return 1;
}
