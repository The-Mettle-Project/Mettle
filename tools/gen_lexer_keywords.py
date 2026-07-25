#!/usr/bin/env python3
"""Re-sort and re-bucket the lexer's keyword tables in src/lexer/lexer.c.

lexer_lex_identifier_or_keyword looks keywords up with a binary search inside a
length bucket, which requires the tables to be grouped by word length and sorted
within each group, and requires the companion `_span` arrays to hold the index
where each length's group starts. Doing that by hand is easy to get wrong and
fails silently -- a misplaced entry just stops being recognised.

So: add or remove entries anywhere in either table, in any order, then run

    python tools/gen_lexer_keywords.py

which rewrites both tables in sorted order, recomputes the spans, updates
LEX_WORD_MAXLEN, and rejects a table it cannot make searchable (a duplicate
word, or an upper-case entry in the case-insensitive table, which a folded
comparison could never match). Pass --check to verify without writing, as CI
does.
"""
import re
import sys

PATH = 'src/lexer/lexer.c'
TABLES = ('g_lex_keywords', 'g_lex_asm_words')
ENTRY = re.compile(r'^\s*\{"([^"]+)",\s*(TOKEN_\w+)\},\s*$')


def parse_table(src, name):
    """Entries of `name`, plus the slice of `src` its declaration occupies."""
    head = 'static const LexWord %s[] = {\n' % name
    start = src.index(head)
    end = src.index('};\n', start) + len('};\n')
    span_head = 'static const unsigned char %s_span[] = {' % name
    span_start = src.index(span_head, end)
    span_end = src.index('\n', span_start) + 1

    entries = []
    for line in src[start + len(head):end].splitlines():
        if line.strip() in ('};', ''):
            continue
        m = ENTRY.match(line)
        if not m:
            sys.exit('%s: cannot parse entry %r' % (name, line))
        entries.append((m.group(1), m.group(2)))
    return entries, start, end, span_start, span_end


def render(name, entries, maxlen):
    entries = sorted(entries, key=lambda wt: (len(wt[0]), wt[0]))
    lines = ['static const LexWord %s[] = {' % name]
    lines += ['    {"%s", %s},' % (w, t) for w, t in entries]
    lines.append('};')

    span, idx = [], 0
    for length in range(0, maxlen + 2):
        while idx < len(entries) and len(entries[idx][0]) < length:
            idx += 1
        span.append(idx)
    return ('\n'.join(lines) + '\n',
            '%s_span[] = {%s};' % (name, ', '.join(str(x) for x in span)))


def main():
    check = '--check' in sys.argv[1:]
    raw = open(PATH, newline='').read()
    crlf = raw.count('\r\n') > raw.count('\n') / 2
    src = raw.replace('\r\n', '\n')

    tables = {name: parse_table(src, name) for name in TABLES}
    every = [e for name in TABLES for e in tables[name][0]]
    maxlen = max(len(w) for w, _ in every)

    for name in TABLES:
        entries = tables[name][0]
        seen = set()
        for word, _ in entries:
            if word in seen:
                sys.exit('%s: duplicate word %r' % (name, word))
            seen.add(word)
        if name == 'g_lex_asm_words':
            upper = [w for w, _ in entries if w != w.lower()]
            if upper:
                sys.exit('g_lex_asm_words: %r must be lower case; the '
                         'case-insensitive lookup folds the input down and '
                         'would never match otherwise' % upper)

    out = src
    # Rewrite back to front so earlier offsets stay valid.
    for name in sorted(TABLES, key=lambda n: -tables[n][1]):
        entries, start, end, span_start, span_end = tables[name]
        body, span = render(name, entries, maxlen)
        out = (out[:span_start] + 'static const unsigned char ' + span +
               '\n' + out[span_end:])
        out = out[:start] + body + out[end:]

    out = re.sub(r'^#define LEX_WORD_MAXLEN \d+$',
                 '#define LEX_WORD_MAXLEN %d' % maxlen, out, count=1, flags=re.M)

    if out == src:
        print('lexer keyword tables are up to date (%d words, max length %d)'
              % (len(every), maxlen))
        return
    if check:
        sys.exit('lexer keyword tables are out of order; run '
                 'tools/gen_lexer_keywords.py')
    open(PATH, 'w', newline='').write(out.replace('\n', '\r\n') if crlf else out)
    print('rewrote %s (%d words, max length %d)' % (PATH, len(every), maxlen))


if __name__ == '__main__':
    main()
