# Control Flow

Mettle provides structured control flow: conditionals, loops, and switches. All control structures use braces for the body.

## Assignment

Assignment uses `=`. The left side must be an lvalue (variable, struct field, array element, or dereferenced pointer). Assignment is a statement; it does not produce a value for use in larger expressions.

```mettle
x = 42;
ptr->field = value;
arr[i] = x;
```

**Compound assignment** (`+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`) is syntactic sugar for `target = target OP value`, where `OP` is the corresponding binary operator. The left side must be the same kind of lvalue as for plain assignment. Compound assignment is a statement, not an expression—it does not produce a value for use in larger expressions. It is valid in `for`-loop initializers and increments.

```mettle
count += 1;
arr[i] *= 2;
for (var i: int32 = 0; i < 10; i += 1) {
  // ...
}
```

See [Lexical Structure](lexical-structure.md#operators-and-punctuation) for the full operator list.

**Type mismatches** produce a compile error. Assigning a value of incompatible type (e.g. `x = 3.14` where `x` is `int32`) is rejected; the compiler does not silently truncate.

## If and Else

The `if` statement evaluates a condition. If true, the then branch runs. The optional `else` branch runs when the condition is false. The condition must be a **numeric type** (integer or floating-point); zero is false, non-zero is true. Pointers are not valid as conditions—use an explicit comparison: `if (ptr != 0)` to check for non-null, not `if (ptr)`.

```mettle
if (x > 0) {
  // ...
} else if (x < 0) {
  // `else if` is parsed as part of the if statement
} else {
  // ...
}
```

`else if` chaining is fully supported as a contiguous sequence of conditions, avoiding deep AST nesting. There is no separate `elseif` keyword.

## While

The `while` loop evaluates the condition. If true, the body runs and the condition is evaluated again. The loop exits when the condition is false.

```mettle
while (condition) {
  // ...
}
```

Common patterns:

```mettle
// Iterate over an array
var i: int32 = 0;
while (i < len) {
  arr[i] = arr[i] * 2;
  i = i + 1;
}

// Infinite loop (e.g. accept loop in a server)
while (1) {
  // ...
}
```

An infinite loop is written `while (1)`; the condition is always true.

## For

The `for` loop has an initializer, condition, and increment. The initializer runs once. The condition is evaluated before each iteration; if false, the loop exits. The increment runs after each iteration. The initializer can declare a variable. Condition and increment are optional—`for (;;)` is a valid infinite loop.

```mettle
for (var i: int32 = 0; i < 10; i = i + 1) {
  // ...
}
```

**Scope:** A variable declared in the initializer (e.g. `var i`) is scoped to the loop. It is not accessible after the loop exits.

**Infinite loop:** Use `for (;;)` when all three parts are omitted. This is idiomatic in systems code.

### Range-based for

`for i in lo..hi { ... }` iterates `i` over a half-open range. `lo..hi` is
**exclusive** of `hi`; `lo..=hi` is **inclusive**:

```mettle
for i in 0..n      { sum = sum + a[i]; }   // i = 0, 1, ..., n-1
for i in 0..=n     { /* i = 0, 1, ..., n */ }
for i: int64 in 0..count { /* loop variable typed explicitly */ }
```

The loop variable's type is inferred from the start bound, or you may annotate
it (`for i: int64 in ...`). A range-based `for` desugars at parse time into the
ordinary counted `for` above: the start bound is evaluated once, the end bound
is re-evaluated each iteration (so hoist a call-valued bound yourself if that
matters). Labels work as usual: `outer: for i in 0..n { ... }`.

> The `..`/`..=` distinction here is exclusive/inclusive. Note that switch-case
> ranges (`case lo..hi:`) use `..` as **inclusive**, a historical inconsistency
> to be aware of.

## Vectorization contracts

A counted loop may carry a `@simd` attribute that asks the optimizer to
vectorize it. This only has effect under `-O` / `--release` (the auto-vectorizer
runs only when optimizing); plain debug builds print one note that the contracts
were not checked.

```mettle
@simd  for i in 0..n { c[i] = a[i] + b[i]; }   // best-effort: warn if not vectorized
@simd! for i in 0..n { c[i] = a[i] + b[i]; }   // contract: compile ERROR if not vectorized
```

- **`@simd`** is a hint. If the loop vectorizes, nothing is printed; if it does
  not, the compiler emits a *warning* explaining why and keeps the scalar loop.
- **`@simd!`** is a hard contract. If the loop does not vectorize, compilation
  **fails** with an error and a precise reason; the performance guarantee
  cannot silently regress.

Both attributes also apply to `while` loops. The diagnostic names the cause when
it can determine it: a function call in the body, control flow (a nested loop or
data-dependent branch), an unsupported element width (16- or 64-bit integers
have no kernel), a loop-carried serial recurrence (a scalar computed from its
own previous value through a non-reassociable operation — `*`, `/`, a shift, or
a bitwise/xor op — so the iterations form a dependency chain, e.g. a hash, an
RNG, or an IIR filter), or, when none of those apply, that no vectorizer
recognized the loop's shape. The recurrence cause is found by backward
data-flow analysis, and `+`/`-` reductions are excluded from it — those
reassociate and vectorize.

`@simd` may also sit on a **function**, where it becomes the default contract
for *every* counted loop in the body that does not carry its own `@simd`:

```mettle
@simd! fn sum(a: int32*, n: int64) -> int64 {
  var s: int64 = 0;
  var i: int64 = 0;
  while (i < n) { s = s + (int64)a[i]; i = i + 1; }   // inherits @simd! from the function
  return s;
}
```

A per-loop attribute always wins over the function default, so you can place a
function-wide `@simd` and still relax (or tighten) an individual loop. Note that
`@simd!` on a function is a hard contract on *all* its counted loops — if the
body mixes vectorizable and non-vectorizable loops, annotate the loops
individually instead. See [Function decorators](declarations.md#function-decorators).

### `--simd-report`

Pass `--simd-report` (with `-O`/`--release`) to have the compiler report what
each `@simd` loop became:

```
kernels.mettle:10:10: note: @simd loop vectorized (simd_dot_i8)
kernels.mettle:21:9:  warning: @simd loop was not vectorized: the loop body contains a function call
```

This makes the optimizer's decision legible instead of a black box: you can see
exactly which kernel a loop lowered to, or why it stayed scalar.

## Switch

The `switch` statement evaluates an expression and compares it to each `case` value. Case values must be compile-time constant integer expressions (including enum variants and `true`/`false`). When a case matches, its body runs. Use `break` to exit the switch. Use `continue` inside a loop that contains the switch to continue the loop. Only one `default` clause is allowed.

**Range cases:** A case may match an inclusive interval with `case lo..hi:`, where both bounds are compile-time constant integer expressions and `lo <= hi`. The case runs when the switch value is in `[lo, hi]`. Cases are tested top to bottom and the first match wins, so a single-value case listed before an overlapping range still takes precedence.

**Fall-through:** Unlike some languages, Mettle does not enforce `break`. If you omit it, execution falls through to the next case (C-style behavior). To avoid accidental bugs, always end each case with `break` explicitly unless you intend fall-through.

**Exhaustiveness:** `switch` over raw integers may omit matching cases and continue after the statement if no case matches. `switch` over `enum` or `bool` must be exhaustive unless a `default` clause is present.

```mettle
switch (expr) {
  case 1:
    // ...
    break;
  case 2:
    // ...
    break;
  case 3..9:        // inclusive range: matches 3 through 9
    // ...
    break;
  default:
    // ...
}
```

## Match

The `match` statement branches on a tagged enum and optionally binds the payload of a variant. The subject expression must have a tagged-enum type.

```mettle
match (value) {
  case Some(v): {
    return v;
  }
  case None: {
    return 0;
  }
}
```

**Arms:** Each `case` arm has a variant name and a block body. Use `case VariantName(binding):` when that variant carries a payload and you want to bind it to a local name. Use `case VariantName:` for payloadless variants.

**Default arm:** `default:` is allowed. Without `default`, the match must cover every variant of the tagged enum.

**No fall-through:** `match` arms do not fall through. Once an arm matches, its block runs and control continues after the `match`.

### match as an expression

`match` also exists in an expression form that yields a value. Each arm body is a single value-yielding expression rather than a block, and arms may be separated by commas, newlines, or semicolons:

```mettle
fn unwrap_or(o: Option, fallback: int32) -> int32 {
  return match (o) {
    case Some(value): value
    case None: fallback
  };
}

var doubled: int32 = match (Some(10)) {
  case Some(v): v
  default: 0
} * 2;
```

All arm bodies must have a compatible type, and because the expression has to produce a value, it must be exhaustive: cover every variant or supply a `default`. The statement form above is the right choice when the arms run several statements or diverge; the expression form is the right choice when each arm is a single value.

## Break and Continue

`break` exits the innermost loop or switch. `continue` skips to the next iteration of the innermost loop. Both are context-checked; they are valid only inside loops or switches. Using them elsewhere is a compile error.

**Important:** `break` and `continue` always target the **innermost** enclosing loop or switch. Inside nested loops, `break` exits only the inner loop. Inside a `switch` that is inside a loop, `break` exits the switch, not the loop—use `continue` to skip to the next loop iteration.

```mettle
while (1) {
  switch (cmd) {
    case 0:
      break;      // exits switch only, loop continues
    case 1:
      continue;   // skips to next loop iteration (exits switch and continues loop)
    case 2:
      break;      // exits switch
  }
  // ...
}
```

### Labeled break and continue

A `while` or `for` loop may carry a label, written `name:` immediately before
the loop keyword. `break name` then exits that labeled loop, and
`continue name` jumps to the next iteration of that labeled loop, regardless of
how deeply nested the statement is:

```mettle
outer: for (var i: int32 = 0; i < n; i = i + 1) {
  for (var j: int32 = 0; j < m; j = j + 1) {
    if (grid[i][j] == target) {
      break outer;     // exits BOTH loops
    }
    if (skip[j]) {
      continue outer;  // next i, abandoning the rest of the j loop
    }
  }
}
```

Rules and limits:

- Labels attach only to `while` and `for` loops. Writing `name:` before any
  other statement is a compile error.
- The label in `break name` / `continue name` must match the label of an
  enclosing loop; an unknown label is a compile error
  (`'break NAME' has no matching labeled loop`).
- `continue name` requires the target to be a loop (every labeled loop is, so
  this always holds for valid labels).
- Unlabeled `break`/`continue` still target the innermost loop or switch as
  before.
- Labels live in their own namespace and do not collide with variable or
  function names.
- Deferred statements are still emitted before the jump, the same as for
  unlabeled `break`/`continue`.

## Return

`return` exits the current function. A function with a return type must provide a value: `return value`. A void function uses `return` with no value.

```mettle
return;
return value;
```

## Short-Circuit Evaluation

Logical operators `&&` and `||` support short-circuit evaluation. For pointer checks like `ptr != 0 && ptr->field > 0`, a single condition is safe:

```mettle
if (ptr != 0 && ptr->field > 0) {
  // ...
}
```

## Defer and Errdefer

`defer` schedules a statement to execute when the current scope exits, while `errdefer` schedules a statement to execute when returning a non-zero value from the current function. Both follow **LIFO (Last In, First Out)** ordering - the most recently deferred statement executes first.

### Syntax and Basic Behavior

Defer statements use the `defer` or `errdefer` keyword followed by a statement:

```mettle
defer cleanup();          // Always runs on scope exit
errdefer rollback();      // Runs on non-zero return
```

The current compiler accepts function calls, assignments, and blocks:

```mettle
defer puts("cleanup");
defer count = count + 1;
defer {
  flush();
  close(handle);
}
errdefer handle_error_recovery();
```

> **Argument capture:** A deferred **direct call** captures its argument values at the defer point (by value). In a loop, `defer print_int(i)` snapshots `i` as it is on that iteration, so the deferred calls see `0, 1, 2, …`, not the final value of `i`. **Method calls** (`defer obj.m(...)`) and **indirect/function-pointer calls** are the exception: they re-evaluate their operands at scope exit, so snapshot into a local first (`var current: int32 = i; defer obj.m(current);`) if you need the defer-point value.

### Implementation Details

**AST Representation:**
- `defer` statements create `AST_DEFER_STATEMENT` nodes
- `errdefer` statements create `AST_ERRDEFER_STATEMENT` nodes
- Both contain a single `statement` field pointing to the deferred statement

**IR Lowering Process:**
1. **Stack Management:** Each scope has an `IRDeferStack` that tracks deferred statements
2. **Scope Hierarchy:** `IRDeferScope` structures form a linked list, allowing nested scopes
3. **Push Operation:** When encountering defer/errdefer, the compiler pushes the AST node onto the current scope's stack with an `is_err` flag
4. **Emission:** At scope exit, the compiler emits deferred statements in reverse order (LIFO)

**Data Structures:**
```c
typedef struct {
    ASTNode *node;    // The defer/errdefer AST node
    int is_err;       // 1 for errdefer, 0 for defer
} IRDeferEntry;

typedef struct {
    IRDeferEntry *entries;
    size_t count;
    size_t capacity;
} IRDeferStack;

typedef struct {
    IRDeferStack stack;
    struct IRDeferScope *parent;  // Link to outer scope
} IRDeferScope;
```

**Return Statement Handling:**
For functions with errdefer statements, the compiler generates two code paths:
1. **Error Path:** Emits both defer and errdefer statements
2. **Success Path:** Emits only defer statements

The return value is checked to determine which path to take, using generated labels like `errdefer_ok_N` and `errdefer_end_N`. This is convention-based: `0` means success and any non-zero return value is treated as an error, so `return 42;` also triggers `errdefer`.

**Control Flow Integration:**
- **Blocks:** Create new `IRDeferScope` with parent link to outer scope
- **If/Else:** Each branch gets its own defer scope; deferred statements run when branch exits
- **Loops:** Each iteration creates a new scope; deferred statements run at iteration end
- **Break/Continue:** Trigger deferred statement emission before jumping

The same success/error split is used for explicit `return` and for implicit fall-through at the end of a function body.

### LIFO Ordering and Execution

Deferred statements execute in reverse order of declaration. This is crucial for resource management where cleanup must happen in reverse of acquisition:

```mettle
func example() {
  defer puts("first");    // Executes third
  defer puts("second");   // Executes second  
  defer puts("third");    // Executes first
  
  // Function body...
  // Output: "third", "second", "first"
}
```

**Mixed defer and errdefer:**
```mettle
func mixed_example() {
  defer puts("always 1");
  errdefer puts("error only");
  defer puts("always 2");
  
  if (error_condition) {
    return err();  // Output: "always 2", "error only", "always 1"
  }
  
  return ok();     // Output: "always 2", "always 1"
}
```

### Scope-Level vs Function-Level Behavior

**Function scope:** defer/errdefer execute when the function returns via any path (return, break from main loop, etc.)

**Block scope:** defer/errdefer execute when the block exits, including if/else branches, loop bodies, and switch cases:

```mettle
func demo() {
  defer puts("function exit");
  
  if (condition) {
    defer puts("if branch exit");  // Runs before function defer
    // ... branch code ...
  } else {
    defer puts("else branch exit");  // Runs before function defer
    // ... else code ...
  }
  
  // Output on condition=true: "if branch exit", "function exit"
  // Output on condition=false: "else branch exit", "function exit"
}
```

### Control Flow Integration

**Loops:** Each iteration gets its own defer scope. Deferred statements run at the end of each iteration. A deferred direct call snapshots its arguments by value on each iteration (see the callout above); method and indirect calls re-evaluate at scope exit.

```mettle
func loop_example() {
  defer puts("function cleanup");
  
  var i: int32 = 0;
  while (i < 3) {
    defer puts("iteration cleanup");  // Runs each iteration
    puts("iteration start");
    i = i + 1;
    
    if (i == 2) {
      break;  // Runs iteration defer, then function defer
    }
  }
  
  // Output: "iteration start", "iteration cleanup", 
  //         "iteration start", "iteration cleanup",
  //         "function cleanup"
}
```

**Switch statements:** Each case that creates a block gets its own defer scope:

```mettle
func switch_demo(value: int32) {
  defer puts("function cleanup");
  
  switch (value) {
    case 1: {
      defer puts("case 1 cleanup");
      // ... case 1 code ...
    }
    case 2: {
      defer puts("case 2 cleanup");
      // ... case 2 code ...
    }
    default: {
      defer puts("default cleanup");
      // ... default code ...
    }
  }
  
  // Only one case's defer runs, plus function defer
}
```

Because `switch` allows fall-through, cleanup order becomes harder to reason about if execution crosses multiple case bodies. Prefer explicit `break` when a case owns deferred cleanup.

**Break and Continue:** These statements trigger deferred statement emission before jumping:

```mettle
func control_flow_demo() {
  defer puts("function cleanup");
  
  while (1) {
    defer puts("iteration cleanup");
    
    if (early_exit) {
      break;  // Runs "iteration cleanup", then "function cleanup"
    }
    
    if (skip_iteration) {
      continue;  // Runs "iteration cleanup", then next iteration
    }
  }
}
```

### Error Handling Patterns

**Resource cleanup with error recovery:**
```mettle
func process_file(filename: string) {
  var file: File* = fopen(filename, "r");
  if (file == 0) {
    return err();  // No defer to run yet
  }
  defer fclose(file);  // Always runs if file was opened
  
  var buffer: uint8* = malloc(4096);
  if (buffer == 0) {
    return err();  // Runs defer: fclose(file)
  }
  errdefer free(buffer);  // Only on error
  
  var data: string = read_file_content(file, buffer, 4096);
  if (data.length == 0) {
    return err();  // Runs errdefer: free(buffer), then defer: fclose(file)
  }
  
  // Process successful data...
  return ok();  // Runs only defer: fclose(file)
}
```

**Nested error handling:**
```mettle
func nested_operations() {
  defer puts("outer cleanup");
  
  var resource1: Resource* = acquire_resource();
  if (resource1 == 0) {
    return err();
  }
  defer release_resource(resource1);
  
  {
    defer puts("inner cleanup");
    
    var resource2: Resource* = acquire_resource();
    if (resource2 == 0) {
      return err();  // Runs "inner cleanup", "release_resource(resource1)", "outer cleanup"
    }
    defer release_resource(resource2);
    
    if (processing_error) {
      return err();  // Runs "release_resource(resource2)", "inner cleanup", 
                   // "release_resource(resource1)", "outer cleanup"
    }
    
    // Success path...
    return ok();  // Runs "release_resource(resource2)", "inner cleanup", 
                   // "release_resource(resource1)", "outer cleanup"
  }
}
```

### Common Pitfalls and Limitations

**Top-level defer:** defer/errdefer can only be used inside functions:

```mettle
// ERROR: defer outside function
defer puts("this fails");

func valid_function() {
  defer puts("this works");  // OK
}
```

**Supported deferred statements:** `defer` and `errdefer` currently support function calls, assignments, and blocks:

```mettle
func example() {
  defer close_file(file);    // OK
  errdefer update_value(x);  // OK
  defer x = 1;               // OK
  errdefer {
    x = x + 1;
    update_value(x);
  }
}
```

**Argument capture:** A deferred direct call snapshots its argument values at the defer point, so each iteration's deferred call sees that iteration's value:

```mettle
while (i < 3) {
  defer print_int(i);  // snapshots i = 0, then 1, then 2
  i = i + 1;
}
```

Method calls and indirect/function-pointer calls are the exception — they re-evaluate at scope exit — so copy into a local first if you need the defer-point value there.

**Performance considerations:** Each defer statement adds runtime overhead for stack management and conditional execution. In performance-critical code, consider manual cleanup for simple cases.

### Resource Management Patterns

**File handling with multiple resources:**
```mettle
func copy_file(src: string, dst: string) {
  var src_file: File* = fopen(src, "r");
  if (src_file == 0) {
    return err();
  }
  defer fclose(src_file);
  
  var dst_file: File* = fopen(dst, "w");
  if (dst_file == 0) {
    return err();
  }
  defer fclose(dst_file);  // Runs first (LIFO)
  
  var buffer: uint8* = malloc(4096);
  if (buffer == 0) {
    return err();
  }
  errdefer free(buffer);
  
  // Copy loop...
  while (!feof(src_file)) {
    var bytes: int32 = fread(buffer, 1, 4096, src_file);
    if (bytes <= 0) {
      return err();  // Free buffer, close dst_file, close src_file
    }
    fwrite(buffer, 1, bytes, dst_file);
  }
  
  free(buffer);  // Manual cleanup before success return
  return ok();     // Close dst_file, close src_file
}
```

**Socket management in servers:**
```mettle
func handle_client_connection(client_socket: int32) {
  defer close_socket(client_socket);
  
  // Set socket options
  if (set_socket_options(client_socket) != 0) {
    return err();  // Runs defer: close_socket(client_socket)
  }
  
  var buffer: uint8* = malloc(8192);
  if (buffer == 0) {
    return err();
  }
  errdefer free(buffer);
  
  // Read request loop
  while (1) {
    var bytes: int32 = recv(client_socket, buffer, 8192, 0);
    if (bytes <= 0) {
      break;  // Client disconnected or error
    }
    
    if (process_request(buffer, bytes) != 0) {
      return err();  // Free buffer, close socket
    }
  }
  
  return ok();  // Free buffer, close socket
}
```

**Memory allocation chains:**
```mettle
func complex_allocation_chain() {
  var resource1: Resource* = allocate_resource();
  if (resource1 == 0) {
    return err();
  }
  defer free_resource(resource1);
  
  var resource2: Resource* = allocate_resource();
  if (resource2 == 0) {
    return err();
  }
  defer free_resource(resource2);
  
  var temp_buffer: uint8* = malloc(1024);
  if (temp_buffer == 0) {
    return err();
  }
  errdefer free(temp_buffer);  // Only on error
  
  if (complex_processing(resource1, resource2, temp_buffer) != 0) {
    return err();  // Free temp_buffer, resource2, resource1
  }
  
  // Success: manually clean up temp_buffer
  free(temp_buffer);
  return ok();     // Free resource2, resource1
}
```

## Unreachable Code
The compiler emits a warning for unreachable statements that appear after an unconditional `return`, `break`, or `continue` in the same block.
