# -*- coding: utf-8 -*-
"""Writes the R One-Seg icon into src/app.rdef.

Same approach and the same isometric axis as R World Radio's tools/make_icon.py,
which this borrows its box helpers from: solid colour styles and closed
straight-edged paths on the 0..64 grid, emitted directly rather than drawn in
Icon-O-Matic.

The subject is a small receiver rather than a page, so it is a full isometric
box - top, left and right faces meeting at the near corner. A screen showing
colour bars sits on the left flank, a stubby antenna leans out of the top, and
the right flank carries a segment strip: thirteen ticks with only the centre
one lit, which is literally what One-Seg is - the middle segment of a
thirteen-segment ISDB-T channel.

Run from the project root:  python3 tools/make_icon.py
It rewrites the `resource vector_icon` block in src/app.rdef in place.
"""

import io
import os
import re


# #pragma mark - HVIF

def _coord(v):
    b = int(round(v)) + 32
    if not 0 <= b <= 127:
        raise ValueError("coordinate %r outside the one-byte range" % v)
    return bytes([b])


def build_hvif(styles, paths, shapes):
    out = bytearray(b"ncif")

    out.append(len(styles))
    for r, g, b, a in styles:
        out.append(0x01)                      # solid colour, RGBA
        out += bytes([r, g, b, a])

    out.append(len(paths))
    for points in paths:
        out.append(0x02 | 0x08)               # closed, no curves
        out.append(len(points))
        for x, y in points:
            out += _coord(x) + _coord(y)

    out.append(len(shapes))
    for style, path_indices in shapes:
        out.append(0x0A)                      # path source
        out.append(style)
        out.append(len(path_indices))
        for i in path_indices:
            out.append(i)
        out.append(0x00)                      # no transform, no flags

    return bytes(out)


def rdef(data):
    lines = ["resource vector_icon {"]
    hexstr = "".join("%02X" % b for b in data)
    for i in range(0, len(hexstr), 64):
        lines.append('\t$"%s"' % hexstr[i:i + 64])
    lines.append("};")
    return "\n".join(lines) + "\n"


# #pragma mark - the box

# A flat panel: wide front (U), tall (DEPTH), and only a thin depth (V), so the
# left face reads as a big flat screen with a shallow 3D edge.
O = (28.0, 7.0)
U = (26.0, 8.0)
V = (-6.0, 2.5)
DEPTH = (0.0, 30.0)


def top(u, v):
    return (O[0] + u * U[0] + v * V[0], O[1] + u * U[1] + v * V[1])


def left(u, h):
    base = top(0, 1)
    return (base[0] + u * U[0] + h * DEPTH[0],
            base[1] + u * U[1] + h * DEPTH[1])


def right(v, h):
    base = top(1, 0)
    return (base[0] + v * V[0] + h * DEPTH[0],
            base[1] + v * V[1] + h * DEPTH[1])


def face(projection, points):
    return [projection(a, b) for a, b in points]


def rect(projection, a0, b0, a1, b1):
    return face(projection, [(a0, b0), (a1, b0), (a1, b1), (a0, b1)])


def shade(color, factor):
    r, g, b, a = color
    return (max(0, min(255, int(r * factor))),
            max(0, min(255, int(g * factor))),
            max(0, min(255, int(b * factor))), a)


def build_box(body_color, behind, groups):
    styles = [body_color, shade(body_color, 0.76), shade(body_color, 0.52)]
    colors = {}

    def style_for(color):
        if color not in colors:
            colors[color] = len(styles)
            styles.append(color)
        return colors[color]

    paths = []
    shapes = []

    def add(style, polys):
        indices = []
        for p in polys:
            indices.append(len(paths))
            paths.append(p)
        if indices:
            shapes.append((style, indices))

    for color, polys in behind:
        add(style_for(color), polys)

    add(1, [[top(0, 1), top(1, 1), left(1, 1), left(0, 1)]])
    add(2, [[top(1, 0), top(1, 1), right(1, 1), right(0, 1)]])
    add(0, [[top(0, 0), top(1, 0), top(1, 1), top(0, 1)]])

    for color, polys in groups:
        add(style_for(color), polys)

    data = build_hvif(styles, paths, shapes)
    return data, len(paths)


# #pragma mark - the flat-panel television
#
# A flat screen viewed slightly from the left: the front (the left face in this
# projection) is almost all screen, showing colour bars, with a thin dark
# bezel and a shallow 3D edge along the top and right. Drawn the BeOS way - solid-colour styles on straight closed paths
# over the 0..64 grid - so it reads as a flat TV at 16 pixels.

CABINET = (54, 58, 66, 255)          # the frame around the screen
BEZEL = (26, 28, 34, 255)            # the black surround

# A SMPTE-style colour-bar test pattern, the one thing that says "television"
# at a glance. Seven vertical bars across the screen.
BARS = [
    (236, 236, 236, 255),            # white
    (230, 206, 96, 255),             # yellow
    (96, 198, 214, 255),             # cyan
    (108, 200, 120, 255),            # green
    (206, 104, 196, 255),            # magenta
    (214, 82, 82, 255),              # red
    (98, 120, 214, 255),             # blue
]


def screen():
    """The front face: a dark bezel almost filling it, and the colour bars
    inside. The bars nearly reach the edges - this is a flat panel, not a boxy
    tube, so the picture dominates."""
    bezel = rect(left, 0.05, 0.05, 0.95, 0.86)
    u0, u1 = 0.10, 0.90
    h0, h1 = 0.12, 0.78
    bars = []
    step = (u1 - u0) / len(BARS)
    for i in range(len(BARS)):
        bars.append(rect(left, u0 + i * step, h0, u0 + (i + 1) * step, h1))
    return bezel, bars


BLOCK = re.compile(r"^resource vector_icon \{.*?^\};\n", re.S | re.M)


if __name__ == "__main__":
    bezel, bars = screen()

    groups = [(BEZEL, [bezel])]
    for color, bar in zip(BARS, bars):
        groups.append((color, [bar]))

    data, count = build_box(CABINET, [], groups)

    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    path = os.path.join(root, "src", "app.rdef")
    text = io.open(path, encoding="utf-8").read()
    if len(BLOCK.findall(text)) != 1:
        raise SystemExit("src/app.rdef: expected exactly one vector_icon block")
    io.open(path, "w", encoding="utf-8").write(
        BLOCK.sub(lambda m: rdef(data), text, count=1))
    print("src/app.rdef: %d bytes, %d paths" % (len(data), count))
