# Modules

Mettle supports a module system. Code is organized into files; one file can import declarations from another. Imports are resolved at compile time and flattened into a single program.

For path resolution, compiler options, and `import_str`, see [Imports](imports.md).

## Import Syntax

The `import` directive takes a string literal path. The path is resolved relative to the current file, or via the stdlib or `-I` directories. The extension `.mettle` is added if omitted.

```mettle
import "module_name";
import "path/to/module";
import "std/io";
```

**Duplicate imports:** If the same file is imported multiple times (e.g. `main.mettle` and `utils.mettle` both import `"std/io"`), the module is included only once. The compiler tracks visited files by canonical path; subsequent imports of the same file are skipped and a warning is emitted: `Circular or duplicate import of 'std/io' (import chain: main.mettle -> utils.mettle -> std/io)`.

**Path case sensitivity:** On Windows, path resolution follows the filesystem (typically case-insensitive). On Linux and macOS, paths are case-sensitive. Use consistent casing (e.g. `"std/io"` not `"Std/IO"`) for portability across platforms.

## Resolution Order

The compiler resolves import paths in this order:

1. **Absolute paths** — used as-is if the path is absolute.
2. **Stdlib** — if the path starts with `std/`, resolve under the stdlib root (default auto-detects bundled stdlib near the compiler binary, then falls back to `./stdlib`; overridable with `--stdlib`).
3. **Relative to importing file** — relative to the directory of the file containing the import.
4. **`-I` directories** — each directory added with `-I` is searched in the order given on the command line.

This means normal projects do not need their own `stdlib/` copy just to import `std/...` modules.

**First match wins:** If two `-I` directories both contain a file matching the same path (e.g. `-I lib1 -I lib2` and both have `foo/bar.mettle`), the file in `lib1` is used. The first directory that yields a readable file wins.

Path separators can be `/` or `\` on Windows.

**Example:** `import "std/io"` resolves under the stdlib root (e.g. `stdlib/std/io.mettle`). `import "io"` would not match the stdlib (no `std/` prefix); it would resolve relative to the current file's directory, or in an `-I` directory that has `io.mettle` at its root. So `"std/io"` and `"io"` are different paths and resolve differently.

## Export

Declarations can be exported with the `export` keyword. Exported declarations are visible to modules that import this file. If a module uses `export` on any declaration, only exported declarations are visible to importers. If a module uses no `export`, all declarations are visible (backward compatibility). This allows modules to hide implementation details by exporting only their public interface.

```mettle
export function forty_two() -> int32 {
  return 42;
}

export var answer: int32 = 42;
export struct Point { ... }
export enum Dir { ... }
export extern function puts(msg: cstring) -> int32 = "puts";
```

**Export applies only to declarations defined in the current file.** You cannot export a forward declaration whose definition lives in another file. The definition and any forward declaration must be in the same module.

**Re-exporting imports:** There is no `export import` syntax. Re-export happens implicitly: when module A imports B, B's exported declarations become part of A's program. When C imports A, C receives both A's exported declarations and B's exported declarations that A imported. You cannot selectively hide an import—anything B exports will be visible to A's importers. The `std/prelude` module uses this pattern: it imports `std/io`, `std/math`, etc., and has no `export`; importers of prelude receive everything from those modules.

**Struct methods:** Exporting a struct automatically makes its methods visible to importers. Methods are part of the struct declaration; no separate export is needed.

## Example

**lib/math.mettle:**

```mettle
export function add(a: int64, b: int64) -> int64 {
  return a + b;
}
```

**main.mettle:**

```mettle
import "lib/math";

function main() -> int32 {
  return add(2, 3);
}
```

Compile with `-I lib` so the compiler can find `lib/math.mettle`:

```bash
mettle -i main.mettle -I lib -o output.obj
```

If the module cannot be resolved, compilation fails with an error such as:

```
Could not resolve imported file 'lib/math' (import chain: main.mettle)
```

The import chain shows the path of imports that led to the failure, which helps when debugging transitive import issues.

## Circular Imports

Circular imports (A imports B, B imports A, or longer cycles) are detected and reported. When A.mettle imports B.mettle and B.mettle imports A.mettle, the compiler emits a warning when processing the second import:

```
Circular or duplicate import of 'A.mettle' (import chain: entry.mettle -> B.mettle -> A.mettle)
```

For a cycle A → B → A, the chain shows the full path: entry imports B, B imports A, A tries to import B again (already visited).

The chain shows the sequence of imports that led to the cycle. The second import of the already-visited file is skipped; the module is included only once. In some configurations this may be reported as a warning rather than a hard error—check your compiler version.

## Import Scope

Imported declarations are flattened into the **global scope**. There are no namespaces or module prefixes. When you `import "std/io"`, the functions and types from that module (e.g. `println`, `print`) become available as bare identifiers. Namespace collision is possible: if two imported modules define the same name, the later one (by import order) wins, and the compiler may report duplicate symbol errors. Use distinct module structure and naming to avoid collisions in larger projects.

## Conditional Imports

There is no way to conditionally import based on platform or compile flags. All `import` directives are unconditional. For platform-specific code (e.g. Windows vs. Unix), use `extern` declarations and link different C implementations, or structure your code so platform-specific logic lives in separate files that you include via your build system (e.g. different entry points per platform).

## Private Declarations

There is no `private` keyword. Visibility is binary: if a module uses `export` on any declaration, only exported declarations are visible; otherwise, everything is visible. To hide implementation details, mark the public API with `export` and leave helper functions, structs, and variables unexported. There is no way to explicitly mark something as "private" in a file that otherwise exports things—omitting `export` achieves that.

## Build System Integration

The compiler takes a **single entry point** (the file passed to `-i`). All imports are resolved transitively from that file. You do not compile each file separately; the compiler follows imports, flattens the program, and produces one assembly output. This differs from C, where you typically compile each `.c` file to an object and link them. With Mettle, a single invocation handles the entire dependency graph. Your build script only needs to run the compiler once with the entry point and appropriate `-I` and `--stdlib` flags.
