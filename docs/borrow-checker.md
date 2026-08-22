# Borrow checker

Mettle's borrow analysis finds pointers that outlive what they point at. It is
pure inference: there is no ownership syntax to write, no lifetime to name, and
nothing to annotate.

It never rejects a program to protect itself. It reports only what it can
prove, which is why there is no escape hatch and no way to silence it.

## What it tracks

A borrow is a pointer derived from something else: `&local`, `&p.field`,
`&buf[4]`, a pointer arithmetic result. The analysis follows each borrow to
its source and asks whether the source is still alive at every use.

Three things end a source's life: its scope closing, a `free` of the block it
sits in, and a `realloc` that may move that block.

## Interior pointer outlives its scope

```mettle
struct P { x: int32; y: int32; }
```

```mettle
var outer: int32* ;
{
  var p: P;
  p.x = 1;
  outer = &p.x;
}
return *outer;
```

```text
warning[M0110]: Use of `outer` after the scope of `p` ended at line 5; `outer`
borrows into `p`, whose storage is reclaimed when its block exits, so this
pointer is dangling
```

The fix is to shorten the pointer's life to the value's scope, or to move the
value out to the wider scope or the heap.

## Invalidated by realloc

```mettle
var base: int32* = malloc(64);
var inner: int32* = &base[4];
base = realloc(base, 128);
return inner[0];
```

```text
warning[M0111]: Use of `inner` after `base` was reallocated at line 6;
`realloc` may move the block, so this pointer is dangling
```

`realloc` is allowed to move the allocation, and it does so unpredictably.
Recompute every interior pointer from the new base after the call.

## Invalidated by free

```mettle
var base: int32* = malloc(64);
var inner: int32* = &base[4];
free(base);
return inner[0];
```

```text
warning[M0112]: Use of `inner` after `base` was freed at line 5; `inner`
borrows into `base`'s block, so this is use-after-free through an interior
pointer
```

Finish with the derived pointers before freeing the base.

## Where it stays quiet

The analysis proves things about code it can follow. It says nothing when it
cannot:

- A pointer whose source came in as a parameter from another translation unit
  or from C.
- A borrow stored in a struct field or an array that then flows through code
  the analysis cannot relate back to the source.
- Anything reached through a `rawptr` after the type is gone.
- Two pointers it cannot prove alias.

That silence is the design. A checker that guessed would produce the false
positives people spend their days fighting, and there would be an annotation to
write to make it stop.

## Relationship to the other checks

The borrow analysis handles derived pointers. The plainer mistakes belong to
the [memory diagnostics](memory-safety.md): a direct use after free is M0101, a
double free is M0102, and returning `&local` is M0103.

`--safe` is a different axis again. It adds bounds checks at run time for
accesses that nothing proved. The borrow analysis costs nothing at run time
and adds no code.

## See also

- [Memory safety](memory-safety.md)
- [Heap allocation](heap-allocation.md)
- [Diagnostics](diagnostics.md)
