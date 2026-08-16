# Heap Allocation

Mettle has no garbage collector. `new`, heap array literals, and string joins
allocate zeroed memory through the owned runtime.

On Windows the runtime asks the process heap for raw storage. On Linux it maps
anonymous pages and manages blocks itself. Neither path calls a C allocator.

Allocation failure returns null. Code that needs a checked policy should test
the result before use.

`std/mem` exports the owned `malloc`, `calloc`, `realloc`, and `free` ABI.
`std/alloc` adds explicit heaps, size classes, and statistics in Mettle code.
`--native-heap` routes generated heap calls through that standard library heap.

Both surfaces are typed with `rawptr`: an allocator hands out an address with
no element type, and a deallocator takes one. `rawptr` converts to and from
every pointer type, so neither `var a: int32* = malloc(n)` nor `free(a)` needs
a cast.

Memory has an explicit lifetime. Free a buffer with the API that created it.
Do not free stack storage or an OS handle. The runtime checks its own allocation
headers but does not add automatic object lifetime or tracing.

The linker always includes the small freestanding runtime object. Section
collection removes services that the program does not call.
