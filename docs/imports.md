# Import System

Mettle has two compile-time import mechanisms:

- `import` loads another Mettle source file and merges the needed declarations into the current compilation.
- `import_str` reads a text file at compile time and embeds it as a `string`.

Both mechanisms use the same path resolver. `import` adds `.mettle` when the path has no extension; `import_str` uses the path exactly as written.

## Module Imports

### Plain Import

```mettle
import "std/io";
import "path/to/module";
import "shared_math";   // resolves shared_math.mettle
```

A plain import brings the module's public surface into the current global scope.

If an imported module has no `export` declarations, every top-level declaration is public for backward compatibility. If it uses `export` anywhere, only exported declarations are public. Private declarations that exported code depends on are still compiled, but the resolver rewrites them to internal names so they are not accidentally callable as source-level globals.

### Namespaced Import

```mettle
import "router" as router;
import "http_util" as http;

if (router.is_get(buf, n) == 1) {
  http.send_404(client);
}
```

`import "..." as alias` exposes the module through `alias.name` instead of adding public names directly to the global scope. The alias is a compile-time namespace, not a runtime object.

Only public members are addressable through the alias. If a public function needs a private helper, that helper is still compiled under the lowered internal name, but `alias.private_helper` will not resolve.

Exported enum variants are namespace members too:

```mettle
import "http_status" as status;

var code: status.Code = status.NotFound;
```

### Selective Import

```mettle
import { send_404, send_all } from "http_util";
import { answer, Pair } from "math_util";

send_404(client);
```

`import { name1, name2 } from "mod"` imports exactly the named top-level declarations into the global scope. The names must be declarations, not enum variants; import the enum declaration and its variants come with it.

When the module uses `export`, every selected name must be exported. Selecting a missing declaration or a private declaration is an import-time error with the import chain attached. The resolver also keeps the selected declarations' dependency closure: functions they call, globals they read, types in signatures and fields, enum payload types, trait bounds, impl targets, casts, `new` expressions, and `match` arms. Dependency declarations are rewritten to internal names unless they were explicitly selected.

## Conditional Imports

An import can be guarded by the host platform:

```mettle
import "std/net" if windows;
import "std/net_posix" if linux;
```

The guard must be `windows` or `linux`. An off-target guarded import is dropped before path resolution, so the referenced file does not need to exist on the other platform. Guards work on plain, namespaced, and selective imports.

## Embedded File Imports

```mettle
var page: string = import_str "index.html";
var config: string = import_str "config/app.txt";
```

`import_str` can appear anywhere a string literal can appear. The embedded value has `.chars` and `.length`, and the bytes are null-terminated for C interop. Unlike `import`, no extension is added automatically.

## Path Resolution

For both `import` and `import_str`, the resolver tries paths in this order:

1. Absolute path.
2. `std/` or `std\` under the configured stdlib root.
3. Package roots from `mettle.deps` files found by walking from the importing file's directory up to the filesystem root.
4. Relative to the importing file.
5. `-I` directories, in command-line order.
6. The path as written, relative to the current working directory.

`std/` imports normally resolve under the bundled stdlib next to the compiler, then fall back to `./stdlib`. Use `--stdlib <dir>` to override the root. On native ELF/Linux builds, stdlib source imports without an extension prefer a `<name>.linux.mettle` sibling when it exists.

`mettle.deps` maps package names to directories:

```text
# mettle.deps
mylib=./packages/mylib
vendor_json=C:/deps/json
```

With that file in or above the importing file's directory, `import "mylib/widget";` resolves under `./packages/mylib/widget.mettle`. Relative dependency roots are interpreted relative to the directory containing that `mettle.deps` file.

## Compiler Options

| Option | Description |
| --- | --- |
| `-I <dir>` | Add an import search directory. Repeatable. |
| `--stdlib <dir>` | Set the stdlib root. |
| `-i <file>` | Set the entry file. A positional input file is also accepted. |

Example:

```bash
mettle -I tests/lib -I vendor main.mettle -o output.obj
```

## Duplicate and Circular Imports

The resolver tracks canonical paths while walking the import graph.

- A duplicate plain import of an already resolved module is skipped.
- A circular import is reported as a warning and the second traversal is skipped.
- Namespaced and selective imports are not treated as duplicate plain imports, because each import shape can expose a different source-level surface.

Diagnostics include the import chain:

```text
Could not resolve imported file 'lib/math' (import chain: main.mettle -> util.mettle)
Could not import 'private_func' from 'math': the declaration is not exported by that module (import chain: main.mettle -> math)
Could not resolve embedded file 'templates/page.html' (import chain: web/server.mettle)
```

## Single Entry Point

Mettle compiles one entry point. Imports are resolved transitively, then the backend receives one combined program. You do not compile each imported source file separately.

## See Also

- [Modules](modules.md) for `export` rules and module visibility.
- [Compilation](compilation.md) for the build pipeline and compiler options.
- [Standard Library](standard-library.md) for stdlib modules and platform notes.
