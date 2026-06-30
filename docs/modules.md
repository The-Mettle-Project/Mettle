# Modules

Mettle code is organized into source files. The compiler starts from one entry file, resolves its imports transitively, then lowers the result into one combined program for the backend.

For path resolution, namespace and selective import details, compiler options, `mettle.deps`, platform guards, and `import_str`, see [Imports](imports.md).

## Import Syntax

```mettle
import "module_name";
import "path/to/module";
import "std/io";
import "path/to/module" as mod;
import { name1, name2 } from "path/to/module";
import "std/net" if windows;
```

Plain imports add public declarations to the global scope. Namespaced imports expose public declarations as `alias.name`. Selective imports add exactly the selected declarations to the global scope and keep any dependency helpers internal.

## Export

Declarations can be exported with the `export` keyword:

```mettle
export fn forty_two() -> int32 {
  return 42;
}

export var answer: int32 = 42;
export struct Point { ... }
export enum Dir { ... }
export extern fn puts(msg: cstring) -> int32 = "puts";
```

If a module has no `export` declarations, every top-level declaration is public for backward compatibility. If a module uses `export` anywhere, only exported declarations are part of its source-level public surface. Non-exported declarations can still be compiled when public declarations depend on them, but the import resolver rewrites those helpers to internal names.

Export applies only to declarations defined in the current file. You cannot export a forward declaration whose definition lives in another file; the declaration and definition must be in the same module.

Exporting a struct makes its methods visible with the struct. Methods do not need separate `export` markers.

## Re-exports

There is no `export import` syntax. Re-export happens through the resolved program:

- If module A imports B and A has no explicit `export`, A's importers receive A's declarations plus the public declarations A imported.
- If A uses `export`, only A's exported declarations are public, plus any public imported declarations that remain part of A's exported surface through normal import resolution.

The `std/prelude` module uses the first pattern: it imports common stdlib modules and has no explicit export list, so importers receive those public declarations.

## Visibility

There is no `private` keyword. A declaration is private by omission when its module uses at least one `export` and that declaration is not marked `export`.

Use `export` for the API you intend other files to call. Keep helper functions, helper structs, and implementation globals unexported. Plain, namespaced, and selective imports all respect that source-level boundary.

## Circular and Duplicate Imports

The resolver tracks canonical file paths while walking the import graph.

- Duplicate plain imports of an already resolved module are skipped.
- Circular imports are reported with the import chain and the repeated traversal is skipped.
- Namespaced and selective imports are not treated as duplicate plain imports, because their source-level surfaces can differ.

Example diagnostic:

```text
Circular import of 'cycle_a' (import chain: main.mettle -> cycle_b -> cycle_a)
```

## Build Integration

The compiler takes one entry point, either with `-i <file>` or a positional input file. You do not compile each imported `.mettle` file separately; one compiler invocation walks the dependency graph and emits the requested output.
