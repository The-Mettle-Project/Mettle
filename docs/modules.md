# Modules and imports

A module is a source file. The compiler starts from one entry file, walks its
imports, and lowers the whole graph into one program. You do not compile
imported files separately.

## Importing

```mettle
import "std/io";
import "lib/helper";
```

A plain import adds the module's public declarations to the current file's
global scope. `import` appends `.mettle` when the path has no extension.

### Namespaced

```mettle
import "lib/helper" as h;
```

```mettle
println("{h.thrice(14)}");
```

The alias is a compile-time namespace and reaches public members only. A
public function's private helper is still compiled, under an internal name,
and `h.secret()` does not resolve.

Exported enum variants are namespace members too, so `status.NotFound` works
after `import "http_status" as status`.

### Selective

```mettle
import { twice } from "lib/helper";
```

Exactly the named top-level declarations enter the global scope. The names
must be declarations; import an enum and its variants come with it.

Selecting a name the module does not export is an error, and the message
carries the import chain. The resolver still pulls in whatever the selection
depends on, functions it calls, globals it reads, types in its signature, and
gives those internal names.

### Platform guards

```mettle
import "std/net" if windows;
import "std/net_posix" if linux;
```

The guard is `windows` or `linux`. An import for the other platform is dropped
before the path is resolved, so that file need not exist on this machine.
Guards work on all three import forms.

### Embedding a file

`import_str` reads a file at compile time and embeds its bytes as a `string`:

```mettle
var page: string = import_str "lib/data.txt";
```

It may appear anywhere a string literal may. The value has `.chars` and
`.length`, and the bytes are nul-terminated so they can cross to C.
`import_str` uses the path exactly as written and adds no extension.

## Exporting

`export` marks a declaration as part of the module's public surface:

```mettle
export fn twice(x: int32) -> int32 { return x * 2; }
export var answer: int32 = 42;
export const MAX_CARGO = 24;
export struct Point { x: int32; y: int32; }
export enum Dir { Up = 1, Down = 2 }
export extern fn puts(msg: cstring) -> int32 = "puts";
```

A module that uses `export` anywhere keeps everything else private. A module
with no `export` at all is entirely public, which is what makes a small
single-file helper work with no ceremony.

Each declaration needs its own `export`, methods included. Exporting a struct
does not export the functions that give it methods:

```mettle
export struct Point { x: int32; y: int32; }
export fn Point_sum(p: Point) -> int32 { return p.x + p.y; }
```

Without the second `export`, an importer holding a `Point` gets:

```text
error[E0003]: Undefined method 'Point.sum' (expected function 'Point_sum')
```

`export` covers declarations in the current file. A forward declaration whose
definition lives elsewhere cannot be exported; both halves belong in one
module.

The `private` keyword is unrelated to visibility. It selects per-work-item
storage inside a [GPU kernel](gpu.md).

## Re-exporting

There is no `export import`. Re-export falls out of resolution: a module with
no `export` of its own passes on both its declarations and the public
declarations it imported. `std/prelude` works that way, which is why importing
it brings the common modules with it.

## Path resolution

For `import` and `import_str` alike, the resolver tries, in order:

1. The path as an absolute path.
2. `std/` under the stdlib root.
3. Package roots named in `mettle.deps` files, found by walking up from the
   importing file.
4. Relative to the importing file.
5. The `-I` directories, in the order given.
6. The path as written, relative to the working directory.

`std/` resolves against the stdlib bundled next to the compiler, then against
`./stdlib`. `--stdlib <dir>` overrides the root. On Linux, a `std/` import
with no extension prefers a `<name>.linux.mettle` sibling when one exists,
which is how `std/io` and `std/net` reach their platform halves.

`mettle.deps` maps a package name to a directory:

```text
mylib=./packages/mylib
vendor_json=C:/deps/json
```

With that file in or above the importing file's directory,
`import "mylib/widget";` resolves to `./packages/mylib/widget.mettle`. A
relative root is read relative to the `mettle.deps` file holding it.

## Duplicate and circular imports

The resolver tracks canonical paths as it walks.

- A second plain import of a resolved module is skipped.
- A circular import is reported and the repeat traversal is skipped.
- A namespaced or selective import is not treated as a duplicate of a plain
  one, because each form exposes a different surface.

Diagnostics carry the chain:

```text
Circular import of 'cycle_a' (import chain: main.mettle -> cycle_b -> cycle_a)
Could not resolve imported file 'lib/math' (import chain: main.mettle -> util.mettle)
```

## Options

| Option | Effect |
|--------|--------|
| `-I <dir>` | Add an import search directory. Repeatable. |
| `--stdlib <dir>` | Set the stdlib root. |
| `--prelude` | Auto-import `std/prelude`, so `println` and friends need no import. |
| `-i <file>` | Name the entry file. A positional path works too. |

```bash
mettle -I tests/lib -I vendor main.mettle -o output.obj
```

## See also

- [Standard library](standard-library.md)
- [Compilation](compilation.md)
- [Declarations](declarations.md)
