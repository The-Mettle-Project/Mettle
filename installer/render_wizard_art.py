"""Render the installer's wizard artwork from the brand mark.

Inno Setup wants one image per display scaling step, so the art is generated
rather than hand-drawn: every size is laid out from the same proportions and
the same palette the site uses, and text is drawn at each size instead of
scaled up from one bitmap.

    python installer/render_wizard_art.py --version=0.17.0

Writes installer/art/*.png plus version.isi, which Mettle.iss includes to
refuse a build whose version does not match the one drawn on the banner.
Requires Pillow, and ImageMagick (`magick`) for the one SVG rasterization.
"""

import os
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(HERE, "art")
MARK_SVG = os.path.join(ROOT, "mettle.svg")

# The site's palette (site/index.html :root).
PAPER = (0xF2, 0xEB, 0xD8)
PAPER_3 = (0xE6, 0xDA, 0xBD)
INK = (0x21, 0x1C, 0x14)
INK_SOFT = (0x57, 0x4E, 0x3D)
INK_FAINT = (0x89, 0x7F, 0x67)
RED = (0xA3, 0x30, 0x1C)
INK_DARK = (0x16, 0x18, 0x1D)

FONTS = "C:/Windows/Fonts/"
SERIF_BOLD = FONTS + "georgiab.ttf"
MONO = FONTS + "consola.ttf"
MONO_BOLD = FONTS + "consolab.ttf"

# Inno picks the file closest to the size it needs: 100/125/150/200% scaling.
BANNER_SIZES = [(164, 314), (205, 393), (246, 471), (328, 628)]
SMALL_SIZES = [(55, 55), (69, 69), (83, 83), (110, 110)]


def version():
    for arg in sys.argv[1:]:
        if arg.startswith("--version="):
            return arg.split("=", 1)[1]
    return "0.17.0"


def mix(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def render_mark(px):
    """Rasterize the brand mark to an RGBA image px wide."""
    tmp = os.path.join(OUT, "_mark.png")
    subprocess.run(
        ["magick", "-background", "none", "-density", "1200", MARK_SVG,
         "-resize", "%dx%d" % (px, px), tmp],
        check=True, capture_output=True)
    mark = Image.open(tmp).convert("RGBA")
    os.remove(tmp)
    return mark


def tint(mark, color):
    """Recolor the mark's opaque pixels, keeping its antialiased edges."""
    solid = Image.new("RGBA", mark.size, color + (255,))
    solid.putalpha(mark.getchannel("A"))
    return solid


def tracked(draw, xy, text, font, fill, spacing):
    """Draw text with extra letter spacing; returns the width drawn."""
    x, y = xy
    for ch in text:
        draw.text((x, y), ch, font=font, fill=fill)
        x += draw.textlength(ch, font=font) + spacing
    return x - spacing - xy[0]


def tracked_width(draw, text, font, spacing):
    return sum(draw.textlength(c, font=font) for c in text) + spacing * (len(text) - 1)


class Theme:
    def __init__(self, ground, wash, ink, rule, frame, accent, faint):
        self.ground = ground
        self.wash = wash
        self.ink = ink
        self.rule = rule
        self.frame = frame
        self.accent = accent
        self.faint = faint


LIGHT = Theme(ground=PAPER, wash=PAPER_3, ink=INK,
              rule=mix(PAPER_3, INK, 0.35), frame=mix(PAPER_3, INK, 0.18),
              accent=RED, faint=INK_FAINT)

DARK = Theme(ground=INK_DARK, wash=INK, ink=PAPER,
             rule=mix(INK_DARK, PAPER, 0.40), frame=mix(INK_DARK, PAPER, 0.22),
             accent=mix(RED, PAPER, 0.35), faint=mix(INK_DARK, PAPER, 0.55))

def banner(w, h, ver, t):
    """The tall image on the Welcome and Finished pages."""
    s = w / 164.0
    img = Image.new("RGB", (w, h), t.ground)
    d = ImageDraw.Draw(img)

    inset = round(13 * s)
    d.rectangle([inset, inset, w - inset - 1, h - inset - 1],
                outline=t.frame, width=max(1, round(s)))

    mark = tint(render_mark(round(74 * s)), t.ink)
    my = round(52 * s)
    img.paste(mark, ((w - mark.width) // 2, my), mark)

    serif = ImageFont.truetype(SERIF_BOLD, max(8, round(26 * s)))
    wy = my + mark.height + round(16 * s)
    wl = d.textlength("Mettle", font=serif)
    d.text(((w - wl) / 2, wy), "Mettle", font=serif, fill=t.ink)

    # Rule + eyebrow: the tagline is the one place the accent color appears.
    ruley = wy + round(38 * s)
    rule_w = round(52 * s)
    d.line([((w - rule_w) / 2, ruley), ((w + rule_w) / 2, ruley)],
           fill=t.rule, width=max(1, round(s)))

    eyebrow = ImageFont.truetype(MONO_BOLD, max(6, round(8.5 * s)))
    tag = "THE NEXT GENERATION"
    track = 1.4 * s
    tagw = tracked_width(d, tag, eyebrow, track)
    tracked(d, ((w - tagw) / 2, ruley + round(11 * s)), tag, eyebrow,
            t.accent, track)

    mono = ImageFont.truetype(MONO, max(6, round(9 * s)))
    vtext = "v" + ver
    vl = d.textlength(vtext, font=mono)
    d.text(((w - vl) / 2, h - inset - round(20 * s)), vtext, font=mono,
           fill=t.faint)

    return img


def small(w, h, t):
    """The header mark on the interior pages."""
    img = Image.new("RGB", (w, h), t.ground)
    mark = tint(render_mark(round(w * 0.78)), t.ink)
    img.paste(mark, ((w - mark.width) // 2, (h - mark.height) // 2), mark)
    return img


def main():
    os.makedirs(OUT, exist_ok=True)
    ver = version()
    written = []

    for w, h in BANNER_SIZES:
        for name, theme in (("WizardImage", LIGHT), ("WizardImageDark", DARK)):
            p = os.path.join(OUT, "%s-%dx%d.png" % (name, w, h))
            banner(w, h, ver, theme).save(p)
            written.append(p)

    for w, h in SMALL_SIZES:
        for name, theme in (("WizardSmallImage", LIGHT),
                            ("WizardSmallImageDark", DARK)):
            p = os.path.join(OUT, "%s-%dx%d.png" % (name, w, h))
            small(w, h, theme).save(p)
            written.append(p)

    # The version is drawn into the banner, so the compile has to fail loudly
    # if the art was rendered for a different one. Mettle.iss includes this and
    # compares it against MyAppVersion.
    stamp = os.path.join(OUT, "version.isi")
    with open(stamp, "w", encoding="ascii") as f:
        f.write("; Written by render_wizard_art.py. Do not edit.\n")
        f.write('#define ArtVersion "%s"\n' % ver)
    written.append(stamp)

    for p in written:
        print("wrote %s" % os.path.relpath(p, ROOT).replace("\\", "/"))


if __name__ == "__main__":
    main()
