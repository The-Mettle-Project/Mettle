#!/usr/bin/env python3
"""Reproducible build of MythicUI's embedded fonts (requires fontTools >= 4.40).

Inputs (official upstream releases, OFL-1.1):
  Geist Serif 1.001  https://github.com/vercel/geist-font/releases/download/v1.9.0/geist-font-v1.9.0.zip
  (release v1.9.0)   -> geist-font/GeistSerif/ttf/GeistSerif-Regular.ttf
  Geist 1.9.0        same zip -> geist-font/Geist/ttf/Geist-{Medium,SemiBold}.ttf
  IBM Plex Mono 2.005 https://github.com/IBM/plex/releases/download/%40ibm%2Fplex-mono%402.5.0/ibm-plex-mono.zip
  (@ibm/plex-mono@2.5.0) -> ibm-plex-mono/fonts/complete/ttf/IBMPlexMono-Regular.ttf
  JetBrains Mono 2.304 https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/JetBrainsMono-2.304.zip
                     -> fonts/ttf/JetBrainsMono-Regular.ttf

Geist Serif ships one weight (Regular 400); its variable font has a shape axis (ELSH) only.
So body text is Geist Serif Regular and the medium and semibold roles (labels, buttons,
titles) use Geist Sans Medium and SemiBold, which share Geist Serif's vertical metrics.

What this does to each font (permitted by OFL-1.1 section 3):
  * subsets to the Unicode ranges in UNICODES (Latin, Greek, Cyrillic, punctuation,
    currency, arrows, common symbols) - MythicUI does no complex shaping (AC: out of scope)
  * removes TrueType hinting bytecode (fpgm/prep/cvt) - MythicUI's rasterizer is unhinted
  * removes GSUB/GPOS/GDEF and, for the Geist faces, replaces GPOS pair kerning with a flat
    legacy 'kern' table (version 0, one format-0 subtable, horizontal) so the engine
    only needs the trivial format-0 binary search for kerning (both monos have no kerning)
  * glyph outlines stay 'glyf' quadratic TrueType; metrics tables unchanged
  * IBM Plex Mono declares the Reserved Font Name "Plex", so the modified font is renamed
    "Mythic Mono Alt" (OFL section 3). Geist, Geist Serif and JetBrains Mono declare
    none and keep their names.

Usage: python3 build_fonts.py <GeistSerif-Regular.ttf> <geist_ttf_dir> <IBMPlexMono-Regular.ttf> <JetBrainsMono-Regular.ttf> <out_dir>
"""
import sys, os
from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0
from fontTools import subset

UNICODES = (
    list(range(0x0020, 0x007F)) +   # Basic Latin
    list(range(0x00A0, 0x0250)) +   # Latin-1, Latin Ext-A, Ext-B
    list(range(0x0370, 0x0400)) +   # Greek
    list(range(0x0400, 0x0500)) +   # Cyrillic
    list(range(0x2000, 0x2070)) +   # General punctuation
    list(range(0x20A0, 0x20C1)) +   # Currency
    list(range(0x2100, 0x2150)) +   # Letterlike (TM, No, etc.)
    list(range(0x2190, 0x2200)) +   # Arrows
    list(range(0x2200, 0x2300)) +   # Math operators
    list(range(0x2300, 0x2400)) +   # Misc technical (cmd, opt, return keys)
    list(range(0x25A0, 0x2600)) +   # Geometric shapes
    [0x2713, 0x2714, 0x2715, 0x2716, 0x2717, 0x2718, 0xFFFD]
)

# Kerning is flattened only between glyphs of these ranges (keeps the table < 10920 pairs).
KERN_RANGES = [(0x0020, 0x007F), (0x00A0, 0x0180), (0x0391, 0x03CA), (0x0410, 0x0450),
               (0x2018, 0x2020), (0x2026, 0x2027), (0x2039, 0x203B)]


def do_subset(src, keep_kern):
    opts = subset.Options()
    opts.hinting = False
    opts.glyph_names = False
    opts.notdef_outline = True
    opts.name_IDs = ['*']
    opts.name_languages = ['*']
    opts.layout_features = ['kern'] if keep_kern else []
    opts.drop_tables += ['gasp', 'DSIG', 'STAT']
    f = TTFont(src)
    s = subset.Subsetter(opts)
    s.populate(unicodes=UNICODES)
    s.subset(f)
    return f


def value_x(v):
    return 0 if v is None else getattr(v, 'XAdvance', 0) or 0


def subtable_fn(st):
    """Return f(left, right) -> (applies, xadv) for one PairPos subtable."""
    if st.Format == 1:
        cov = {g: i for i, g in enumerate(st.Coverage.glyphs)}
        sets = st.PairSet
        cache = {}
        def f(a, b):
            i = cov.get(a)
            if i is None:
                return (False, 0)
            if i not in cache:
                cache[i] = {r.SecondGlyph: value_x(r.Value1) for r in sets[i].PairValueRecord}
            d = cache[i]
            if b in d:
                return (True, d[b])
            return (False, 0)
        return f
    if st.Format == 2:
        cov = set(st.Coverage.glyphs)
        c1 = st.ClassDef1.classDefs if st.ClassDef1 else {}
        c2 = st.ClassDef2.classDefs if st.ClassDef2 else {}
        recs = st.Class1Record
        def f(a, b):
            if a not in cov:
                return (False, 0)
            return (True, value_x(recs[c1.get(a, 0)].Class2Record[c2.get(b, 0)].Value1))
        return f
    return lambda a, b: (False, 0)


def flatten_kern(font):
    if 'GPOS' not in font:
        return {}
    gpos = font['GPOS'].table
    idx = set()
    for fr in gpos.FeatureList.FeatureRecord:
        if fr.FeatureTag == 'kern':
            idx.update(fr.Feature.LookupListIndex)
    lookups = []
    for li in sorted(idx):
        lk = gpos.LookupList.Lookup[li]
        fns = []
        for st in lk.SubTable:
            if lk.LookupType == 9:
                if st.ExtensionLookupType != 2:
                    continue
                st = st.ExtSubTable
            elif lk.LookupType != 2:
                continue
            fns.append(subtable_fn(st))
        if fns:
            lookups.append(fns)
    cmap = font.getBestCmap()
    glyphs = sorted({cmap[u] for lo, hi in KERN_RANGES for u in range(lo, hi) if u in cmap})
    pairs = {}
    for a in glyphs:
        for b in glyphs:
            total = 0
            for fns in lookups:
                for fn in fns:
                    ok, v = fn(a, b)
                    if ok:
                        total += v
                        break
            if total:
                pairs[(a, b)] = total
    return pairs


def strip_layout(font):
    for t in ('GSUB', 'GPOS', 'GDEF'):
        if t in font:
            del font[t]


def add_kern(font, pairs):
    # drop imperceptible pairs (< 1/128 em = 0.1 px at 13 px)
    lim = font['head'].unitsPerEm / 128
    pairs = {k: v for k, v in pairs.items() if abs(v) >= lim}
    if len(pairs) > 10920:
        # one format-0 subtable must stay < 64 KiB: keep ASCII-ASCII pairs first, then
        # pairs touching Latin-1/Latin Ext-A, then the rest; strongest first within a tier
        rev = {}
        for u, g in font.getBestCmap().items():
            rev.setdefault(g, u)
        def tier(kv):
            (a, b), v = kv
            ua, ub = rev.get(a, 0xFFFF), rev.get(b, 0xFFFF)
            if ua < 0x80 and ub < 0x80:
                t = 0
            elif ua < 0x180 and ub < 0x180:
                t = 1
            else:
                t = 2
            return (t, -abs(v))
        pairs = dict(sorted(pairs.items(), key=tier)[:10920])
    k = newTable('kern')
    k.version = 0
    st = KernTable_format_0()
    st.version = 0
    st.coverage = 1  # horizontal, kerning values, no cross-stream, no override
    st.format = 0
    st.kernTable = pairs
    st.apple = False
    k.kernTables = [st]
    font['kern'] = k
    return len(pairs)


# OFL section 3: a modified font may not use a Reserved Font Name.
RENAME = {'IBM Plex Mono': 'Mythic Mono Alt', 'IBMPlexMono': 'MythicMonoAlt'}
NAME_IDS = (1, 3, 4, 6, 16, 17, 18, 21, 22, 25)


def rename(font):
    for r in font['name'].names:
        if r.nameID in NAME_IDS:
            v = r.toUnicode()
            for a, b in RENAME.items():
                v = v.replace(a, b)
            r.string = v


def save(f, dst, n=None):
    f.save(dst)
    k = '' if n is None else f', {n} kern pairs'
    print(dst, os.path.getsize(dst), 'bytes,', len(f.getGlyphOrder()), f'glyphs{k}')


def main():
    serif, geist_dir, plex, jbm, out = sys.argv[1:6]
    os.makedirs(out, exist_ok=True)
    f = do_subset(serif, True)
    pairs = flatten_kern(f)
    strip_layout(f)
    n = add_kern(f, pairs)
    save(f, os.path.join(out, 'GeistSerif-Regular.ttf'), n)
    for w in ('Medium', 'SemiBold'):
        f = do_subset(os.path.join(geist_dir, f'Geist-{w}.ttf'), True)
        pairs = flatten_kern(f)
        strip_layout(f)
        n = add_kern(f, pairs)
        save(f, os.path.join(out, f'Geist-{w}.ttf'), n)
    f = do_subset(plex, False)
    strip_layout(f)
    rename(f)
    save(f, os.path.join(out, 'MythicMonoAlt-Regular.ttf'))
    f = do_subset(jbm, False)
    strip_layout(f)
    save(f, os.path.join(out, 'JetBrainsMono-Regular.ttf'))


if __name__ == '__main__':
    main()
