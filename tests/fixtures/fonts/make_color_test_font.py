# SPDX-License-Identifier: MIT
"""Builds ColorTest.ttf, the COLRv0 fixture for the color glyph tests (issue #12).

Run with fontTools installed: python make_color_test_font.py ColorTest.ttf

The font maps U+1F600 to a "face": a yellow disc with two black square eyes, as COLRv0 layers.
U+1F601 is a single red square, also layered, so a stack test can tell the glyphs apart. All
outlines are authored here (MIT, like the rest of the fixtures); nothing is derived from another
font. The output is deterministic for a given fontTools version.
"""

import math
import sys

from fontTools.colorLib.builder import buildCOLR, buildCPAL
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

UPEM = 1000


def square(x0, y0, x1, y1):
    pen = TTGlyphPen(None)
    pen.moveTo((x0, y0))
    pen.lineTo((x0, y1))
    pen.lineTo((x1, y1))
    pen.lineTo((x1, y0))
    pen.closePath()
    return pen.glyph()


def disc(cx, cy, r, sides=24):
    pen = TTGlyphPen(None)
    for i in range(sides):
        angle = -2 * math.pi * i / sides  # clockwise for TrueType
        point = (round(cx + r * math.cos(angle)), round(cy + r * math.sin(angle)))
        if i == 0:
            pen.moveTo(point)
        else:
            pen.lineTo(point)
    pen.closePath()
    return pen.glyph()


def main(out):
    names = [".notdef", "face", "face.disc", "face.eyes", "box", "box.fill"]
    builder = FontBuilder(UPEM, isTTF=True)
    builder.setupGlyphOrder(names)
    builder.setupCharacterMap({0x1F600: "face", 0x1F601: "box"})
    empty = TTGlyphPen(None).glyph()
    # The base glyphs keep a plain outline so a renderer without COLR support still draws something.
    builder.setupGlyf(
        {
            ".notdef": square(100, 0, 500, 700),
            "face": disc(500, 350, 400),
            "face.disc": disc(500, 350, 400),
            "face.eyes": square(300, 450, 400, 550),
            "box": square(100, 0, 900, 800),
            "box.fill": square(100, 0, 900, 800),
        }
    )
    # The second eye is a second contour of the same layer glyph.
    eyes = TTGlyphPen(None)
    for x0, x1 in ((300, 400), (600, 700)):
        eyes.moveTo((x0, 450))
        eyes.lineTo((x0, 550))
        eyes.lineTo((x1, 550))
        eyes.lineTo((x1, 450))
        eyes.closePath()
    builder.font["glyf"]["face.eyes"] = eyes.glyph()
    del empty
    # TrueType places an outline at xMin - lsb, so each glyph's left side bearing must equal its
    # own xMin or the COLR layers would drift apart.
    glyf = builder.font["glyf"]
    metrics = {}
    for name in names:
        glyph = glyf[name]
        glyph.recalcBounds(glyf)
        metrics[name] = (1000, getattr(glyph, "xMin", 0))
    builder.setupHorizontalMetrics(metrics)
    builder.setupHorizontalHeader(ascent=900, descent=-200)
    builder.setupNameTable({"familyName": "Tessera Color Test", "styleName": "Regular"})
    builder.setupOS2(sTypoAscender=900, sTypoDescender=-200, usWinAscent=900, usWinDescent=200)
    builder.setupPost()
    builder.font["CPAL"] = buildCPAL([[(1.0, 0.85, 0.0, 1.0), (0.0, 0.0, 0.0, 1.0), (0.9, 0.1, 0.1, 1.0)]])
    builder.font["COLR"] = buildCOLR(
        {
            "face": [("face.disc", 0), ("face.eyes", 1)],
            "box": [("box.fill", 2)],
        },
        version=0,
    )
    builder.save(out)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "ColorTest.ttf")
