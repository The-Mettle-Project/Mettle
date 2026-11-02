# Settle

A settlement job posts card records to 64 merchant accounts. Record 1,500,000
is corrupt: it names a merchant that does not exist, and the job faults there.
You fix the code while the job waits, and it carries on from that record with
everything it has posted so far.

```bash
cd examples/settle
mettle live settle.mettle -s -- 12000000 20
```

The arguments are the record count and a pause in milliseconds per 2,000
records, which stands in for the database write a real job would do. This run
takes two to three minutes, enough to make the edits below by hand. `-s` makes a
fault name its exact line and column.
Without arguments the job posts 500,000 records at full speed and exits, which
is what the examples harness does.

## 1. The fault

About 20 seconds in:

```text
1500000 records, 122784319 dollars settled
Fatal error: array index out of bounds (index 9001, length 64)
  --> settle.mettle:34:32 in post
   |
34 |     book[merchant].sales = book[merchant].sales + cents;
   |                                ^ index 9001 is out of bounds (0..63)
[live] main stopped at the fault. Save a fix and it continues from its last quiesce point
```

## 2. The fix

Add this to `post`, under its first line, and save:

```mettle
    if (merchant >= 64) {
        println("record {record} held: merchant {merchant} does not exist");
        return;
    }
```

```text
Patch 1: post (2304 bytes)
[live] patch 1: 1 function
[live] main continues from its last quiesce point
record 1500000 held: merchant 9001 does not exist
```

`main` runs `quiesce;` once per record, so the job retries the record that
faulted and posts nothing twice. A job that quiesced once per batch would post
the start of that batch again.

## 3. A new field

The job is still running and you want refund counts. Add a field:

```mettle
struct Merchant {
    sales: int64
    count: int64
    refunds: int64
}
```

Count it in `post`, after the line that adds to `count`:

```mettle
    if (cents < 0) {
        book[merchant].refunds = book[merchant].refunds + 1;
    }
```

Add it up in `report`:

```mettle
    var refunds: int64 = 0;
```

```mettle
        refunds = refunds + book[m].refunds;
```

```mettle
    println("{done} records, {settled / 100} dollars settled, {refunds} refunds");
```

Save once the three edits are in. The accounts convert in place and keep their
balances. Refunds count from the moment the patch lands.

```text
Patch 2: post, report, checksum, main (15529 bytes, 1 heap type converted to a new layout)
[live] patch 2: 4 functions, 1 heap object converted
[live] main moves to the current code
[live] main: 'book' keeps its value in its new layout
3000000 records, 245512784 dollars settled, 7273 refunds
```

## 4. Undo

From another terminal:

```bash
mettle --live-undo settle.exe
```

On Linux the program is `settle`. The accounts convert back, again with their
balances, and the report loses its refund column.

```text
Patch 3 undoes patch 2
[live] patch 3 undoes patch 2
[live] main moves to the current code
```

## 5. Check the answer

The job ends with a checksum over every merchant's sales and count:

```text
12000000 records, 981788871 dollars settled
checksum 12645556001838
```

Build the source you ended with and run it from record zero at full speed. It
reaches the same dollars and the same checksum, and its report still has the
refund column, since undo changes the program and leaves your file as it is.

```bash
mettle --build settle.mettle -o clean.exe
./clean.exe 12000000
```

[Live programs](../../docs/live.md) covers what a patch can change;
[the design](../../docs/design/live.md) covers how.
