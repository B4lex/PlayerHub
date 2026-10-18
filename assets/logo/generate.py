#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["fonttools==4.66.1", "skia-pathops==0.9.2"]
# ///
"""Builds assets/fonts/PlayerHub.ttf from logo.svg, discord.svg, and the icons.

The overlay draws the logos and icons as glyphs of this font, so Dear ImGui
rasterizes them at any size: U+E000 is the white letters and U+E001 the orange
bar with the black scratches cut out of it, U+E002 the Discord symbol scaled to
the same height, and U+E003 onwards the ICONS, each scaled so its SVG height is
the em. Run it with `uv run --script`.

An icon SVG holds paths filled or stroked (round caps and joins) in black or
white, painted in order: black adds to the glyph and white cuts it away, so the
SVG looks like the glyph in a browser.
"""

import pathlib
import xml.etree.ElementTree as ElementTree

import pathops
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.svgLib.path import parse_path

HERE = pathlib.Path(__file__).resolve().parent
SOURCE = HERE / "logo.svg"
DISCORD_SOURCE = HERE / "discord.svg"
ICONS_DIRECTORY = HERE.parent / "icons"
ICONS = {0xE003: "muted.svg", 0xE004: "vanished.svg"}
OUTPUT = HERE.parent / "fonts" / "PlayerHub.ttf"

LETTERS = "white"
BAR = "#ff9000"
SCRATCHES = "black"
DISCORD = "#5865f2"

# TrueType coordinates are integers; four units per SVG unit keep rounding
# under a tenth of a pixel at any size the window draws.
SCALE = 4
# Stroke conics become quadratics within this many SVG units.
ICON_CONIC_TOLERANCE = 0.01
# Curves become quadratic within this many font units.
MAXIMUM_ERROR = 1.0


def read_paths(source, fills):
    root = ElementTree.parse(source).getroot()
    width, height = (float(root.get(name)) for name in ("width", "height"))
    paths = {}
    for element in root.iter("{http://www.w3.org/2000/svg}path"):
        fill = element.get("fill", "").lower()
        if element.get("transform") or element.get("fill-rule") not in (None, "nonzero"):
            raise SystemExit(f"{source.name}: only untransformed nonzero paths are supported")
        path = pathops.Path()
        parse_path(element.get("d"), path.getPen())
        paths.setdefault(fill, []).append(path)
    unknown = set(paths) - fills
    if unknown:
        raise SystemExit(f"{source.name}: unexpected fills {sorted(unknown)}")
    return width, height, paths


def read_icon(source):
    root = ElementTree.parse(source).getroot()
    width, height = (float(root.get(name)) for name in ("width", "height"))
    glyph = pathops.Path()
    for element in root.iter("{http://www.w3.org/2000/svg}path"):
        if element.get("transform") or element.get("fill-rule") not in (None, "nonzero"):
            raise SystemExit(f"{source.name}: only untransformed nonzero paths are supported")
        fill = element.get("fill", root.get("fill", "black")).lower()
        stroke = element.get("stroke", root.get("stroke", "none")).lower()
        if (fill == "none") == (stroke == "none") or {fill, stroke} - {"black", "white", "none"}:
            raise SystemExit(f"{source.name}: a path is either filled or stroked, in black or white")
        for name in ("stroke-linecap", "stroke-linejoin"):
            if stroke != "none" and element.get(name, root.get(name)) != "round":
                raise SystemExit(f"{source.name}: strokes need {name}=\"round\"")
        path = pathops.Path()
        parse_path(element.get("d"), path.getPen())
        if stroke != "none":
            path.stroke(float(element.get("stroke-width", root.get("stroke-width", "1"))),
                pathops.LineCap.ROUND_CAP, pathops.LineJoin.ROUND_JOIN, 4)
            # Round caps and joins come out as conics, which the path operations reject.
            path.convertConicsToQuads(ICON_CONIC_TOLERANCE)
        color = stroke if stroke != "none" else fill
        operation = pathops.PathOp.UNION if color == "black" else pathops.PathOp.DIFFERENCE
        glyph = pathops.op(glyph, path, operation)
    return width, height, glyph


def flip(path, height, scale=SCALE):
    flipped = pathops.Path()
    path.draw(TransformPen(flipped.getPen(), (scale, 0, 0, -scale, 0, height * scale)))
    # TrueType fills nonzero; outer contours run clockwise by convention.
    flipped.simplify(clockwise=True)
    return flipped


def to_glyph(path):
    pen = TTGlyphPen(None)
    path.draw(Cu2QuPen(pen, MAXIMUM_ERROR, reverse_direction=False))
    return pen.glyph()


def main():
    width, height, paths = read_paths(SOURCE, {LETTERS, BAR, SCRATCHES})
    letters = pathops.Path()
    pathops.union(paths[LETTERS], letters.getPen())
    bar = pathops.Path()
    pathops.difference(paths[BAR], paths.get(SCRATCHES, []), bar.getPen())

    advance = round(width * SCALE)
    em = round(height * SCALE)

    discord_width, discord_height, discord_paths = read_paths(DISCORD_SOURCE, {DISCORD})
    discord = pathops.Path()
    pathops.union(discord_paths[DISCORD], discord.getPen())
    discord_scale = em / discord_height
    glyphs = {
        ".notdef": TTGlyphPen(None).glyph(),
        "space": TTGlyphPen(None).glyph(),
        "letters": to_glyph(flip(letters, height)),
        "bar": to_glyph(flip(bar, height)),
        "discord": to_glyph(flip(discord, discord_height, discord_scale)),
    }
    advances = {name: advance for name in glyphs}
    advances["discord"] = round(discord_width * discord_scale)
    characters = {0x20: "space", 0xE000: "letters", 0xE001: "bar", 0xE002: "discord"}
    for character, file in ICONS.items():
        icon_width, icon_height, icon = read_icon(ICONS_DIRECTORY / file)
        name = pathlib.Path(file).stem
        icon_scale = em / icon_height
        glyphs[name] = to_glyph(flip(icon, icon_height, icon_scale))
        advances[name] = round(icon_width * icon_scale)
        characters[character] = name

    # The em is the logo's height, so a font size in pixels is its height.
    builder = FontBuilder(em, isTTF=True)
    builder.setupGlyphOrder(list(glyphs))
    builder.setupCharacterMap(characters)
    builder.setupGlyf(glyphs)
    glyf = builder.font["glyf"]
    builder.setupHorizontalMetrics({name: (advances[name], getattr(glyf[name], "xMin", 0)) for name in glyphs})
    builder.setupHorizontalHeader(ascent=em, descent=0)
    builder.setupNameTable({"familyName": "PlayerHub", "styleName": "Regular"})
    builder.setupOS2(
        sTypoAscender=em, sTypoDescender=0, sTypoLineGap=0, usWinAscent=em, usWinDescent=0
    )
    builder.setupPost()
    # A fixed timestamp makes the file the same on every run.
    builder.updateHead(created=0, modified=0)
    builder.font.recalcTimestamp = False
    builder.save(OUTPUT)
    print(f"{OUTPUT.relative_to(HERE.parent.parent)}: {advance}x{em} units")


if __name__ == "__main__":
    main()
