# Design

## The reader

One state machine reads every dialect. Its states are those of Python's `_csv.c`: start of
record, start of field, in an unquoted field, escaped byte, in a quoted field, quote seen in a
quoted field, escaped byte in a quoted field. Rust's csv-core has the same machine (minus the
unquoted escape). Lenient repair of bad quoting follows both, and the Python and csv-core test
cases check that it does.

A record is parsed in one call over the bytes from the record start. If the buffer ends inside
the record and more input may come, the call returns without changing the reader; the reader
then keeps the partial record, reads more (growing the buffer when it is full, so a long record
costs O(n) in total) and parses the record again. Line and column numbers are kept as the
bytes go by: CR, LF and CRLF each count as one line break.

### Finding special bytes

Most bytes in a CSV file are plain data. The reader builds a bitmap of the bytes that may be
special, 64 bytes at a time: one bit per byte, set for the delimiter, the quote, the escape
byte, a custom terminator, and every byte below 14 (CR and LF among them). Then it walks the set
bits, taking the lowest bit each time (count trailing zeros, by de Bruijn multiplication), and
never looks at the bytes in between. This is the structural index of simdcsv (Langdale and
Lemire) and of zsv. They build it with SSE/AVX/NEON compares and a movemask; Mettle has no
vector types, so here each 8-byte word is compared with all the keys at once in a 64-bit
register (SWAR):

```text
t = word ^ (key * 0x0101010101010101)          zero bytes where word has the key
(t - 0x0101010101010101) & ~t & 0x8080...80    high bit set in the first zero byte
```

A borrow can set extra high bits after a real match, and "below 14" also flags tabs and other
control bytes. Both only add bits; every set bit is checked against the real byte, so extra
bits cost a little time and never change a result. The 8 high bits of a word are gathered into
one byte with a multiply by `0x0002040810204081`.

The bitmap of the current block is kept between records, so each byte is classified once.

### Fast path

For the common dialect (a quote byte, doubled quotes, no escape byte, CR/LF record ends, no
initial-space skipping) a short loop walks the bitmap and handles unquoted fields and quoted
fields that have no line break inside. Anything else (a quote in the middle of a field, data
after a closing quote, a line break inside quotes) makes it give up on that record, and the
full state machine parses the record again from its start. So the fast path never decides
anything the state machine would decide differently; the conformance tests run both.

### Zero copy

A field is a view into the input buffer when its bytes are the field's bytes: unquoted fields
with no escapes, and quoted fields with no doubled quotes or escapes. Only fields that need
rewriting are built in a scratch buffer. The scratch buffer may move while a record is parsed,
so such fields hold an offset until the record is finished.

## The writer

A field is scanned eight bytes at a time for anything that forces quoting; most fields are
written with one copy. The rules are Python's `csv.writer` QUOTE_MINIMAL rules, checked byte
for byte against Python on random rows, plus two that keep round trips exact: a first field
starting with the comment byte is quoted, and so is a field starting with a space when the
dialect skips initial spaces.

## Numbers

- Integers: decimal with an optional sign, checked for overflow before each digit.
- Text to float: digits are collected (at most 768 significant digits; beyond that a sticky
  digit stands for the rest, which is enough to round correctly). Up to 15 digits with a power
  of ten up to 22 use Clinger's exact fast path: one float multiply or divide. Everything else
  uses exact big-integer arithmetic: the value is scaled to a 64-bit quotient plus a remainder,
  then rounded half to even, with subnormals handled. float32 uses the same code with a 23-bit
  mantissa, so it rounds once, from the exact value.
- Float to text: the shortest digits that read back to the same float, by the Burger and Dybvig
  free-format algorithm with exact big integers, laid out like Python's `repr`.

## Typed rows

Mettle reflects on struct fields at compile time (`comptime for f in typeof(T).fields`). The
loop cannot yet run inside a generic function, so the user's code runs it once to build a
schema: field name, kind, offset and size. The library decodes into and encodes from the
struct's bytes with that schema. Columns are bound by header name once, then each record is
decoded by index.
