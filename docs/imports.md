# Import System

Mettle provides two mechanisms for bringing external content into your program at compile time: **module imports** (`import`) and **embedded file imports** (`import_str`). Both use the same path resolution rules.

## Module Import: `import`

The `import` directive loads another Mettle source file and makes its declarations available in the current program.

```mettle
import "std/io";
import "path/to/module";
import "shared_math";   // .mettle added if omitted
```

- **Path format:** A string literal. Forward slashes (`/`) or backslashes (`\`) are accepted on Windows.
- **Extension:** If the path has no extension, `.mettle` is appended automatically. `"std/io"` resolves to `std/io.mettle`.
- **Scope:** Imported declarations are flattened into the **global scope** by default. Use `import "…" as alias` or `import { … } from "…"` (see below) to control the namespace and the set of names brought in.

### Namespaced Imports

`import "…" as alias` prefixes every exported name from the module with `alias.`, keeping your global scope clean:

```mettle
import "router" as router;
import "http_util" as http;

// Call site
if (router.is_get(buf, n) == 1) {
  http.send_404(client);
}
```

Names from a namespaced import are accessed only through the alias — they are not added to the global scope directly.

### Selective Imports

`import { name1, name2 } from "mod"` pulls exactly the named declarations (plus any internal helpers they depend on) into the global scope — no alias, no extra names:

```mettle
import { send_404, send_all } from "http_util";
import { serve_forum_index, count_threads } from "forum";

// Use directly — only the listed names land in scope
send_404(client);
var n: int32 = count_threads(mutex);
```

Selective imports work regardless of whether the module uses `export`. When the module does use `export`, only exported names may be selected (private helpers are still excluded unless they are transitively required by a selected name).

### Conditional Imports

An import may carry a platform guard so it applies to only one target:

```mettle
import "std/net" if windows;        // resolved only on a Windows build
import "std/net.linux" if linux;    // resolved only on a Linux build
```

A guarded import is included only when its platform matches the build target, and an off-target guarded module is never looked up — so a platform-specific module that does not exist on the other OS is safe to reference. The guard applies to plain, namespaced, and selective imports; the predicate must be `windows` or `linux`. Imports without a guard are unconditional. The compiler targets its host, so the active platform follows the host (ELF/Linux versus COFF/Windows).

### What Can Be Imported

For plain `import "…"` and `import "…" as alias`, only **exported** declarations are visible when a module uses `export`. If a module does not use `export`, all of its declarations are visible (backward compatibility). See [Modules](modules.md) for export rules.

## Embedded File Import: `import_str`

The `import_str` expression reads a file at compile time and embeds its contents as a `string` value. Use it for HTML, config files, or any text data.

```mettle
var PAGE_CONTENT: string = import_str "index.html";
var config: string = import_str "config.txt";
```

- **Path format:** Same as `import` — a string literal.
- **Extension:** No automatic extension. The path is used as given. `"index.html"` resolves to `index.html`, not `index.html.mettle`.
- **Result:** The file contents become a `string` with `.chars` (pointer to data) and `.length` (byte count). The string is null-terminated for C interop.

`import_str` can appear anywhere a string literal is valid: variable initializers, function call arguments, etc.

## Path Resolution Order

Both `import` and `import_str` resolve paths in this order:

1. **Absolute paths** — Used as-is. On Windows: `C:\path\to\file` or `\path\to\file`. On Unix: `/path/to/file`.

2. **Stdlib** — If the path starts with `std/` (or `std\` on Windows), it is resolved under the stdlib root (default auto-detects bundled stdlib near the compiler binary, then falls back to `./stdlib`; overridable with `--stdlib`).
   - `import "std/io"` → `stdlib/std/io.mettle`
   - `import_str "std/template.html"` → `stdlib/std/template.html`

You do not need a project-local `stdlib/` directory for `std/...` imports in normal usage.

3. **`mettle.deps` package roots** — For imports that are not absolute and do not use the `std/` prefix, the compiler walks **from the importing file’s directory up toward the filesystem root** and collects every `mettle.deps` file on that path (memoized per directory). Each file maps a **package name** (the segment before the first `/` or `\` in the import path) to a **directory root**.
   - Line format: `name=value`. Lines starting with `#` or empty lines are ignored. `value` may be absolute, or relative to the directory that contains this `mettle.deps`.
   - Example: if `mettle.deps` in the repo root contains `mylib=./packages/mylib`, then `import "mylib/widget"` resolves under `./packages/mylib/widget` (with `.mettle` appended when omitted), evaluated **before** resolution relative to the importing file or `-I` paths.

4. **Relative to importing file** — Relative to the directory of the file containing the import.
   - In `web/server.mettle`, `import_str "index.html"` → `web/index.html`
   - In `main.mettle`, `import "lib/utils"` → `lib/utils.mettle` (relative to `main.mettle`’s directory)

5. **`-I` directories** — Each directory added with `-I` is searched in command-line order.
   - `mettle -I lib -I vendor main.mettle` — `lib` is searched before `vendor`.
   - First match wins: if both `lib/foo.mettle` and `vendor/foo.mettle` exist, `lib/foo.mettle` is used.

6. **Fallback** — The path is tried as-is (e.g. current working directory).

### Path Separators

On Windows, both `/` and `\` are accepted. Use `/` for portability across platforms.

## Compiler Options

| Option | Description |
|--------|--------------|
| `-I <dir>` | Add import search directory. Repeatable. |
| `--stdlib <dir>` | Set stdlib root (default: bundled auto-detect, then `./stdlib`). |
| `-i <file>` | Input file (entry point). Imports are resolved from this file. |

**Example:**

```bash
mettle -I tests/lib -I vendor main.mettle -o output.s
```

This allows `import "shared_math"` to resolve to `tests/lib/shared_math.mettle` when `tests/lib` is on the search path.

## Duplicate and Circular Imports

- **Duplicate imports:** If the same file is imported multiple times (e.g. via different import paths), it is included only once. A warning is emitted:
  ```
  Circular or duplicate import of 'std/io' (import chain: main.mettle -> utils.mettle -> std/io)
  ```

- **Circular imports:** Cycles (A → B → A) are detected. The second import of an already-visited file is skipped. The import chain in the message shows the path that led to the cycle.

## Error Messages and Import Chain

When a path cannot be resolved, the compiler reports the **import chain** — the sequence of files that led to the failed import:

```
Could not resolve imported file 'lib/math' (import chain: main.mettle -> utils.mettle)
```

```
Could not resolve embedded file 'templates/forum.css' (import chain: web/server.mettle)
```

```
Could not read embedded file 'missing.txt' (import chain: main.mettle)
```

The chain helps debug transitive import issues and missing files.

## Single Entry Point

The compiler takes one entry point (the file passed via `-i` or as a positional argument). All imports are resolved transitively from that file. You do not compile each file separately; a single invocation processes the full dependency graph and produces one assembly output.

## See Also

- [Modules](modules.md) — Export rules, visibility, re-exports
- [Compilation](compilation.md) — Build pipeline, options
- [Standard Library](standard-library.md) — Stdlib layout, `std/` modules
