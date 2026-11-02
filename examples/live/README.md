# Live

Four ships fly, burn fuel and dock while you change the program under them.

```bash
cd examples/live
mettle live live.mettle -- 100000
```

The argument is the number of frames, at 20 a second. Without it the program
runs 200 frames at full speed and exits, which is what the examples harness
does.

With the program running, save these edits to `live.mettle` one at a time.

## 1. Change a function

In `steer`, change `ship->x + 1.5` to `ship->x + 3.0`. On the next frame the
lead ship's `x` climbs twice as fast.

```text
Patch 1: steer (678 bytes)
[live] patch 1: 1 function
```

## 2. Change a struct

Add a field to `Ship`, move it in `steer`, and print it in `report`:

```mettle
struct Ship {
    x: float64
    y: float64
    fuel: float64
    heading: float64
}
```

```mettle
    ship->heading = ship->heading + 0.5;
```

```mettle
    println("frame {frame} lead x={fleet[0].x} y={fleet[0].y} fuel={fleet[0].fuel} docked={docked} heading={fleet[0].heading}");
```

The fleet lives on the heap. It converts in place, keeps every position and
fuel level, and the new field starts at zero. `main` moves to the new code in
the middle of its loop.

```text
Patch 2: steer, report, main (9964 bytes, 1 heap type converted to a new layout)
[live] patch 2: 3 functions, 1 heap object converted
[live] main moves to the current code
```

## 3. Crash it

In `report`, change `fleet[0].x` to `fleet[4].x`. The bounds check fires and
the program waits for a fix:

```text
Fatal error: array index out of bounds (index 4, length 4)
  --> live.mettle:29:1 in report
[live] main stopped at the fault. Save a fix and it continues from its last quiesce point
```

Change it back and save. The frame counter carries on from where it stopped.

```text
[live] main continues from its last quiesce point
```

Run with `-s` (`mettle live live.mettle -s -- 100000`) for the exact line and
column of a fault.

## 4. Undo

From another terminal:

```bash
mettle --live-undo live.exe
```

On Linux the program is `live`. This reverts the latest patch. Undo the fix
from step 3 and the fault comes back, and the program waits again.

## 5. Two at once

Stop `mettle live`, build the program yourself, and start it in two
terminals:

```bash
mettle --build --live live.mettle -o live.exe
./live.exe 100000
```

Both copies read the same patches. Change `ship->y + 0.5` to `ship->y + 2.0`
in `steer`, pick a frame a few seconds ahead of the later copy, and patch from
a third terminal:

```bash
mettle --live-patch live.exe --live-at-tick 600 live.mettle
```

```text
Patch 1: steer (678 bytes, at tick 600)
```

Each copy speeds up on its own frame 600.

[Live programs](../../docs/live.md) covers what a patch can change;
[the design](../../docs/design/live.md) covers how.
