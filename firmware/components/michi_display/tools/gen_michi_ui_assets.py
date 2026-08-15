#!/usr/bin/env python3
"""Michi UI asset generator: bitmap fonts + icons + theme verification.

Generates (committed, build-time data only - no runtime dependency):
  assets/fonts/michi_ui_fonts_data.h
  assets/icons/michi_ui_icons_data.h

Design decisions (documented; see also michi_ui_fonts.h / michi_ui_icons.h):
  * Fonts are PROPORTIONAL (per-glyph advance table), 1-bit column-major
    bitmaps (LSB = top row), stored const in flash.
  * XS (8 px em): hand-designed bitmap art (PIL TrueType hinting at 8 px
    produces broken strokes; pixel-level art is the only clean path).
  * SM (11 px): Google Sans Medium  (bolder strokes survive 11 px hinting).
  * MD (14 px) / LG (20 px) / PIN (36 px): Google Sans Regular.
  * Charset: ASCII 0x20..0x7E plus '…' (UTF-8 E2 80 A6, full-index 95).
    PIN: digits + space + '.' only (PIN map remaps everything else).
  * Icons are procedural stroke art rasterized with 4x4 supersampling;
    stroke width scales with the target size for a consistent look.

Usage: python3 tools/gen_michi_ui_assets.py [--preview DIR]
Requires: Pillow (dev-only; the generated headers are committed).
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

try:
    import PIL
    PIL_VER = PIL.__version__
except ImportError:
    PIL_VER = "?"

FONT_DIR = "/usr/share/fonts/TTF"
FONT_REGULAR = f"{FONT_DIR}/GoogleSans-Regular.ttf"
FONT_MEDIUM = f"{FONT_DIR}/GoogleSans-Medium.ttf"

HERE = os.path.dirname(os.path.abspath(__file__))
FONTS_OUT = os.path.join(HERE, "..", "assets", "fonts", "michi_ui_fonts_data.h")
ICONS_OUT = os.path.join(HERE, "..", "assets", "icons", "michi_ui_icons_data.h")

# ---------------------------------------------------------------------------
# Theme palette (RGB888 sources) -> RGB565 with proper rounding.
# The generated values must match michi_ui_theme.h (smoke test verifies).
# ---------------------------------------------------------------------------
PALETTE = [
    ("MICHI_UI_BG", 0x08, 0x0A, 0x0F),
    ("MICHI_UI_SURFACE", 0x10, 0x14, 0x1A),
    ("MICHI_UI_SURFACE_ELEVATED", 0x15, 0x1A, 0x24),
    ("MICHI_UI_TEXT_PRIMARY", 0xF5, 0xF6, 0xF8),
    ("MICHI_UI_TEXT_SECONDARY", 0x9A, 0xA3, 0xB3),
    ("MICHI_UI_TEXT_TERTIARY", 0x66, 0x70, 0x80),
    ("MICHI_UI_ACCENT", 0xFF, 0x5C, 0x8A),
    ("MICHI_UI_ACCENT_SOFT", 0xE8, 0x4F, 0x7B),
    ("MICHI_UI_SUCCESS", 0x5A, 0xD6, 0xA0),
    ("MICHI_UI_WARNING", 0xF2, 0xB8, 0x5B),
    ("MICHI_UI_ERROR", 0xFF, 0x62, 0x62),
    ("MICHI_UI_INFO", 0x68, 0xA8, 0xFF),
    ("MICHI_UI_MUTED", 0x50, 0x57, 0x65),
]


def rgb565(r, g, b):
    """Round-trip conversion (nearest, not truncation)."""
    r5 = (r * 31 + 127) // 255
    g6 = (g * 63 + 127) // 255
    b5 = (b * 31 + 127) // 255
    return (r5 << 11) | (g6 << 5) | b5


# ---------------------------------------------------------------------------
# Stroke rasterizer (shared by icons; unit coords, y down).
# ---------------------------------------------------------------------------

def _seg_dist(px, py, x0, y0, x1, y1):
    dx, dy = x1 - x0, y1 - y0
    l2 = dx * dx + dy * dy
    if l2 <= 0.0:
        return math.hypot(px - x0, py - y0)
    t = ((px - x0) * dx + (py - y0) * dy) / l2
    t = max(0.0, min(1.0, t))
    return math.hypot(px - (x0 + t * dx), py - (y0 + t * dy))


def _flatten(prims, pts=24):
    """Expand primitives into (segs, discs, polys).

    segs: stroke segments (capsule, radius r)
    discs: filled discs (radius rad + r*0.8 for a soft edge)
    polys: filled polygons (fill + stroke outline for rounded corners)
    """
    segs = []
    discs = []
    polys = []
    for p in prims:
        kind = p[0]
        if kind == "L":
            segs.append((p[1], p[2], p[3], p[4]))
        elif kind == "C":  # circle stroke
            cx, cy, rad = p[1], p[2], p[3]
            last = None
            for i in range(pts + 1):
                a = 2.0 * math.pi * i / pts
                x, y = cx + rad * math.cos(a), cy + rad * math.sin(a)
                if last:
                    segs.append((last[0], last[1], x, y))
                last = (x, y)
        elif kind == "A":  # arc stroke (degrees, ccw in math coords)
            cx, cy, rad, a0, a1 = p[1], p[2], p[3], p[4], p[5]
            last = None
            for i in range(pts + 1):
                a = math.radians(a0 + (a1 - a0) * i / pts)
                x, y = cx + rad * math.cos(a), cy + rad * math.sin(a)
                if last:
                    segs.append((last[0], last[1], x, y))
                last = (x, y)
        elif kind == "B":  # cubic bezier stroke
            x0, y0, c1x, c1y, c2x, c2y, x1, y1 = p[1:]
            last = (x0, y0)
            for i in range(1, pts + 1):
                t = i / pts
                mt = 1.0 - t
                x = (mt ** 3 * x0 + 3 * mt ** 2 * t * c1x +
                     3 * mt * t ** 2 * c2x + t ** 3 * x1)
                y = (mt ** 3 * y0 + 3 * mt ** 2 * t * c1y +
                     3 * mt * t ** 2 * c2y + t ** 3 * y1)
                segs.append((last[0], last[1], x, y))
                last = (x, y)
        elif kind == "D":  # filled disc
            discs.append((p[1], p[2], p[3]))
        elif kind == "F":  # filled polygon (with rounded outline)
            polys.append(p[1])
        elif kind == "P":  # polygon outline (rounded joins)
            poly = p[1]
            n = len(poly)
            for i in range(n):
                x0, y0 = poly[i]
                x1, y1 = poly[(i + 1) % n]
                segs.append((x0, y0, x1, y1))
                discs.append((x0, y0, 0.0))  # vertex disc, radius = stroke r
        elif kind == "R":  # rounded-rect stroke
            x, y, w, h, rad = p[1:]
            x2, y2 = x + w, y + h
            segs.append((x + rad, y, x2 - rad, y))
            segs.append((x2 - rad, y2, x + rad, y2))
            segs.append((x, y + rad, x, y2 - rad))
            segs.append((x2, y + rad, x2, y2 - rad))
            for cx, cy, a0, a1 in ((x + rad, y + rad, 180, 270),
                                   (x2 - rad, y + rad, 270, 360),
                                   (x2 - rad, y2 - rad, 0, 90),
                                   (x + rad, y2 - rad, 90, 180)):
                last = None
                for i in range(pts // 4 + 1):
                    a = math.radians(a0 + (a1 - a0) * i / (pts // 4))
                    px, py = cx + rad * math.cos(a), cy + rad * math.sin(a)
                    if last:
                        segs.append((last[0], last[1], px, py))
                    last = (px, py)
        elif kind == "Rf":  # filled rounded rect
            x, y, w, h, rad = p[1:]
            polys.append([(x + rad, y), (x + w - rad, y), (x + w, y + rad),
                          (x + w, y + h - rad), (x + w - rad, y + h),
                          (x + rad, y + h), (x, y + h - rad), (x, y + rad)])
        else:
            raise ValueError(f"unknown primitive {kind}")
    return segs, discs, polys


def _inside_poly(px, py, poly):
    inside = False
    n = len(poly)
    j = n - 1
    for i in range(n):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if (yi > py) != (yj > py) and px < (xj - xi) * (py - yi) / (yj - yi) + xi:
            inside = not inside
        j = i
    return inside


def rasterize(prims, size, stroke):
    r = (stroke / 2.0) / size  # unit space
    segs, discs, polys = _flatten(prims)
    poly_edges = []
    for poly in polys:
        n = len(poly)
        for i in range(n):
            x0, y0 = poly[i]
            x1, y1 = poly[(i + 1) % n]
            poly_edges.append((x0, y0, x1, y1))

    def covered(u, v):
        for (x0, y0, x1, y1) in segs:
            if _seg_dist(u, v, x0, y0, x1, y1) <= r:
                return True
        for (cx, cy, rad) in discs:
            if math.hypot(u - cx, v - cy) <= rad + r * 0.8:
                return True
        for poly in polys:
            if _inside_poly(u, v, poly):
                return True
        for (x0, y0, x1, y1) in poly_edges:
            if _seg_dist(u, v, x0, y0, x1, y1) <= r:
                return True
        return False

    sub = 4
    rows = []
    for py in range(size):
        row = []
        for px in range(size):
            hits = 0
            for sy in range(sub):
                for sx in range(sub):
                    u = (px + (sx + 0.5) / sub) / size
                    v = (py + (sy + 0.5) / sub) / size
                    if covered(u, v):
                        hits += 1
            row.append(1 if hits * 2 >= sub * sub else 0)
        rows.append(row)
    return rows


def art(rows):
    out = []
    for row in rows:
        out.append("".join("#" if c else "." for c in row))
    return out


def pack_rows(rows):
    """Row-major, 1 bpp, MSB first."""
    out = bytearray()
    w = len(rows[0])
    for row in rows:
        acc = 0
        cnt = 0
        for c in row:
            acc = (acc << 1) | (1 if c else 0)
            cnt += 1
            if cnt == 8:
                out.append(acc)
                acc = 0
                cnt = 0
        if cnt:
            out.append(acc << (8 - cnt))
    return bytes(out)


# ---------------------------------------------------------------------------
# Icon definitions (unit box, y down)
# ---------------------------------------------------------------------------
CAT = [("C", 0.50, 0.58, 0.28),
       ("L", 0.27, 0.32, 0.21, 0.06), ("L", 0.21, 0.06, 0.40, 0.24),
       ("L", 0.73, 0.32, 0.79, 0.06), ("L", 0.79, 0.06, 0.60, 0.24)]
WIFI = [("A", 0.50, 0.64, 0.50, 225, 315),
        ("A", 0.50, 0.64, 0.30, 225, 315),
        ("A", 0.50, 0.64, 0.10, 225, 315),
        ("D", 0.50, 0.88, 0.06)]
SERVER = [("R", 0.18, 0.12, 0.64, 0.76, 0.07),
          ("L", 0.26, 0.40, 0.74, 0.40),
          ("L", 0.26, 0.60, 0.74, 0.60)]
SPEAKER = [("R", 0.20, 0.14, 0.60, 0.46, 0.06),
           ("C", 0.50, 0.78, 0.15),
           ("D", 0.50, 0.78, 0.05)]
PLAY = [("F", [(0.30, 0.16), (0.30, 0.84), (0.78, 0.50)])]
PAUSE = [("Rf", 0.28, 0.16, 0.16, 0.68, 0.03),
         ("Rf", 0.56, 0.16, 0.16, 0.68, 0.03)]
PAIR = [("C", 0.40, 0.42, 0.23), ("C", 0.60, 0.58, 0.23)]
BUTTON = [("C", 0.50, 0.50, 0.30), ("C", 0.50, 0.50, 0.13)]
WARNING = [("P", [(0.50, 0.06), (0.93, 0.85), (0.07, 0.85)]),
           ("L", 0.50, 0.38, 0.50, 0.62),
           ("D", 0.50, 0.72, 0.05)]
ERROR = [("C", 0.50, 0.50, 0.30),
         ("L", 0.30, 0.30, 0.70, 0.70),
         ("L", 0.70, 0.30, 0.30, 0.70)]
UPDATE = [("L", 0.16, 0.78, 0.84, 0.78),
          ("L", 0.50, 0.14, 0.50, 0.60),
          ("L", 0.30, 0.42, 0.50, 0.62),
          ("L", 0.70, 0.42, 0.50, 0.62)]
WAVE = [("A", 0.32, 0.50, 0.16, 0, 180),
        ("A", 0.64, 0.50, 0.16, 180, 360)]

ICONS = [
    ("cat", CAT),
    ("wifi", WIFI),
    ("server", SERVER),
    ("speaker", SPEAKER),
    ("play", PLAY),
    ("pause", PAUSE),
    ("pair", PAIR),
    ("button", BUTTON),
    ("warning", WARNING),
    ("error", ERROR),
    ("update", UPDATE),
    ("wave", WAVE),
]

# (name, sizes) - all icons at 12/20/32; the cat also 24 and 48.
ICON_SIZES = {name: [12, 20, 32] for name, _ in ICONS}
ICON_SIZES["cat"] = [12, 20, 24, 32, 48]

ICON_STROKE = {12: 1.0, 20: 1.6, 24: 1.9, 32: 2.6, 48: 3.8}

# ---------------------------------------------------------------------------
# XS font: hand-designed 8-row bitmap art.
# Grid: rows 0..5 = cap height, rows 2..5 = x-height, rows 6..7 = descender.
# 1 px stroke, proportional advance = width + 1 (space = 3, thin = w + 1).
# ---------------------------------------------------------------------------
XS_ART = {
    0x20: ["...", "...", "...", "...", "...", "...", "...", "..."],
    0x21: ["#", "#", "#", "#", "#", ".", "#", "."],
    0x22: ["#.#", "#.#", "...", "...", "...", "...", "...", "..."],
    0x23: [".#.#.", "#####", ".#.#.", ".#.#.", "#####", ".#.#.", ".....", "....."],
    0x24: ["..#..", ".###.", "#.#..", ".##.", "..#.#", ".###.", "..#..", "....."],
    0x25: ["##..#", "##.#.", "...#.", "..#..", ".#...", "#..##", ".....", "....."],
    0x26: [".##..", "#..#.", "#..#.", ".##..", "#.#..", ".####", ".....", "....."],
    0x27: ["#", "#", ".", ".", ".", ".", ".", "."],
    0x28: [".#", "#.", "#.", "#.", "#.", ".#", "..", ".."],
    0x29: ["#.", ".#", ".#", ".#", ".#", "#.", "..", ".."],
    0x2A: ["..#..", "#.#.#", ".###.", "#.#.#", "..#..", ".....", ".....", "....."],
    0x2B: [".....", "..#..", "..#..", "#####", "..#..", "..#..", ".....", "....."],
    0x2C: ["..", "..", "..", "..", "..", ".#", "#.", ".."],
    0x2D: ["...", "...", "...", "###", "...", "...", "...", "..."],
    0x2E: [".", ".", ".", ".", ".", "#", ".", "."],
    0x2F: ["...#", "..#.", "..#.", ".#..", ".#..", "#...", "....", "...."],
    0x30: [".##.", "#..#", "#..#", "#..#", "#..#", ".##.", "....", "...."],
    0x31: [".#.", "##.", ".#.", ".#.", ".#.", "###", "...", "..."],
    0x32: [".##.", "#..#", "...#", "..#.", ".#..", "####", "....", "...."],
    0x33: [".##.", "#..#", "...#", "..#.", "#..#", ".##.", "....", "...."],
    0x34: ["...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", ".....", "....."],
    0x35: ["####", "#...", "###.", "...#", "#..#", ".##.", "....", "...."],
    0x36: [".##.", "#...", "###.", "#..#", "#..#", ".##.", "....", "...."],
    0x37: ["####", "...#", "..#.", "..#.", ".#..", ".#..", "....", "...."],
    0x38: [".##.", "#..#", ".##.", "#..#", "#..#", ".##.", "....", "...."],
    0x39: [".##.", "#..#", "#..#", ".###", "...#", ".##.", "....", "...."],
    0x3A: [".", ".", "#", ".", ".", "#", ".", "."],
    0x3B: ["..", "..", ".#", "..", "..", ".#", "#.", ".."],
    0x3C: ["...#", "..#.", ".#..", "#...", ".#..", "..#.", "...#", "...."],
    0x3D: ["....", "....", "####", "....", "####", "....", "....", "...."],
    0x3E: ["#...", ".#..", "..#.", "...#", "..#.", ".#..", "#...", "...."],
    0x3F: [".##.", "#..#", "...#", "..#.", "....", "..#.", "....", "...."],
    0x40: [".###.", "#...#", "#.###", "#.#.#", "#.###", ".###.", ".....", "....."],
    0x41: [".##..", "#..#.", "#..#.", "####.", "#..#.", "#..#.", ".....", "....."],
    0x42: ["###..", "#..#.", "#..#.", "###..", "#..#.", "###..", ".....", "....."],
    0x43: [".###.", "#....", "#....", "#....", "#....", ".###.", ".....", "....."],
    0x44: ["###..", "#..#.", "#..#.", "#..#.", "#..#.", "###..", ".....", "....."],
    0x45: ["####", "#...", "#...", "###.", "#...", "####", "....", "...."],
    0x46: ["####", "#...", "#...", "###.", "#...", "#...", "....", "...."],
    0x47: [".###.", "#....", "#....", "#.###", "#..#.", ".###.", ".....", "....."],
    0x48: ["#..#.", "#..#.", "#..#.", "####.", "#..#.", "#..#.", ".....", "....."],
    0x49: ["###", ".#.", ".#.", ".#.", ".#.", "###", "...", "..."],
    0x4A: ["..##", "...#", "...#", "...#", "#..#", ".##.", "....", "...."],
    0x4B: ["#..#.", "#.#..", "##...", "##...", "#.#..", "#..#.", ".....", "....."],
    0x4C: ["#...", "#...", "#...", "#...", "#...", "####", "....", "...."],
    0x4D: ["#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#", ".....", "....."],
    0x4E: ["#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", ".....", "....."],
    0x4F: [".###.", "#...#", "#...#", "#...#", "#...#", ".###.", ".....", "....."],
    0x50: ["###..", "#..#.", "#..#.", "###..", "#....", "#....", ".....", "....."],
    0x51: [".###.", "#...#", "#...#", "#.#.#", "#..#.", ".##.#", ".....", "....."],
    0x52: ["###..", "#..#.", "#..#.", "###..", "#.#..", "#..#.", ".....", "....."],
    0x53: [".###.", "#....", "#....", ".###.", "....#", ".###.", ".....", "....."],
    0x54: ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", ".....", "....."],
    0x55: ["#...#", "#...#", "#...#", "#...#", "#...#", ".###.", ".....", "....."],
    0x56: ["#...#", "#...#", "#...#", "#...#", ".#.#.", "..#..", ".....", "....."],
    0x57: ["#...#", "#...#", "#...#", "#.#.#", "#.#.#", ".#.#.", ".....", "....."],
    0x58: ["#...#", ".#.#.", "..#..", "..#..", ".#.#.", "#...#", ".....", "....."],
    0x59: ["#...#", ".#.#.", "..#..", "..#..", "..#..", "..#..", ".....", "....."],
    0x5A: ["#####", "...#.", "..#..", ".#...", "#....", "#####", ".....", "....."],
    0x5B: ["##.", "#..", "#..", "#..", "#..", "##.", "...", "..."],
    0x5C: ["#...", ".#..", ".#..", "..#.", "..#.", "...#", "....", "...."],
    0x5D: [".##", "..#", "..#", "..#", "..#", ".##", "...", "..."],
    0x5E: ["..#..", ".#.#.", "#...#", ".....", ".....", ".....", ".....", "....."],
    0x5F: ["....", "....", "....", "....", "....", "....", "####", "...."],
    0x60: ["#.", ".#", "..", "..", "..", "..", "..", ".."],
    0x61: ["....", "....", ".##.", "...#", ".###", "#..#", "....", "...."],
    0x62: ["#...", "#...", "##..", "#..#", "#..#", ".###", "....", "...."],
    0x63: ["....", "....", ".##.", "#...", "#...", ".##.", "....", "...."],
    0x64: ["...#", "...#", ".###", "#..#", "#..#", ".###", "....", "...."],
    0x65: ["....", "....", ".##.", "#..#", "####", "#..#", "....", "...."],
    0x66: ["..##", ".#..", "###.", ".#..", ".#..", ".#..", "....", "...."],
    0x67: ["....", "....", ".##.", "#..#", "#..#", ".###", "...#", ".##."],
    0x68: ["#...", "#...", "##..", "#..#", "#..#", "#..#", "....", "...."],
    0x69: ["#.", "..", "#.", "#.", "#.", "#.", "..", ".."],
    0x6A: ["..#", "...", "..#", "..#", "..#", "..#", "#.#", ".#."],
    0x6B: ["#...", "#...", "#.#.", "##..", "##..", "#..#", "....", "...."],
    0x6C: ["#.", "#.", "#.", "#.", "#.", "#.", "..", ".."],
    0x6D: [".....", ".....", ".###.", "#.#.#", "#.#.#", "#.#.#", ".....", "....."],
    0x6E: ["....", "....", "##..", "#..#", "#..#", "#..#", "....", "...."],
    0x6F: ["....", "....", ".##.", "#..#", "#..#", ".##.", "....", "...."],
    0x70: ["....", "....", "##..", "#..#", "#..#", ".###", "#...", "#..."],
    0x71: ["....", "....", ".##.", "#..#", "#..#", ".###", "...#", "...#"],
    0x72: ["...", "...", "##.", "#.#", "#..", "#..", "...", "..."],
    0x73: ["....", "....", ".###", "#...", ".##.", "..##", "....", "...."],
    0x74: ["....", ".#..", "###.", ".#..", ".#..", "..#.", "....", "...."],
    0x75: ["....", "....", "#..#", "#..#", "#..#", ".###", "....", "...."],
    0x76: [".....", ".....", "#...#", "#...#", "#...#", ".#.#.", "..#..", "....."],
    0x77: [".....", ".....", "#...#", "#...#", "#.#.#", "#.#.#", ".#.#.", "....."],
    0x78: [".....", ".....", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "....."],
    0x79: [".....", ".....", "#...#", "#...#", "#...#", ".###.", "..#..", ".#..."],
    0x7A: ["....", "....", "####", "..#.", ".#..", "####", "....", "...."],
    0x7B: ["..#", ".#.", ".#.", "#..", ".#.", ".#.", "..#", "..."],
    0x7C: ["#", "#", "#", "#", "#", "#", "#", "#"],
    0x7D: ["#..", ".#.", ".#.", "..#", ".#.", ".#.", "#..", "..."],
    0x7E: [".....", ".....", ".##.#", "#.##.", ".....", ".....", ".....", "....."],
    0xE2_80_A6: [".....", ".....", ".....", ".....", ".....", "#.#.#", ".....", "....."],
}


def xs_glyph_art(code):
    """Return normalized XS art: 8 rows, uniform width, and the advance."""
    rows = XS_ART[code]
    assert len(rows) == 8, f"XS glyph {code:#x} has {len(rows)} rows"
    w = max(len(r) for r in rows)
    rows = [r.ljust(w, ".") for r in rows]
    if code == 0x20:
        adv = 3
    else:
        adv = w + 1
    return rows, w, adv


# ---------------------------------------------------------------------------
# PIL-backed fonts (SM/MD/LG/PIN)
# ---------------------------------------------------------------------------

PIL_FONTS = {
    "sm": (FONT_MEDIUM, 11),
    "md": (FONT_REGULAR, 14),
    "lg": (FONT_REGULAR, 20),
    "pin": (FONT_REGULAR, 36),
}

FULL_CHARS = "".join(chr(c) for c in range(0x20, 0x7F)) + "\u2026"
PIN_CHARS = "0123456789 ."


def pil_glyph(font, ch, box_h):
    """Render one glyph into the shared em box (top = ascent row 0)."""
    tmp = Image.new("L", (600, box_h + 4))
    d = ImageDraw.Draw(tmp)
    d.text((0, 0), ch, fill=255, font=font)
    bbox = d.textbbox((0, 0), ch, font=font)
    adv = max(1, int(round(d.textlength(ch, font=font))))
    if bbox is None or bbox[2] <= bbox[0]:
        return [], 0, adv
    w = bbox[2] - bbox[0]
    glyph = tmp.crop((bbox[0], 0, bbox[2], box_h)).point(
        lambda p: 255 if p >= 128 else 0)
    cols = []
    for x in range(w):
        col = 0
        for y in range(box_h):
            if glyph.getpixel((x, y)):
                col |= 1 << y
        cols.append(col)
    return cols, w, adv


# ---------------------------------------------------------------------------
# Emit helpers
# ---------------------------------------------------------------------------

def _fmt_array_body(items, per_line=12):
    out = []
    for i in range(0, len(items), per_line):
        out.append("    " + ", ".join(items[i:i + per_line]) + ",")
    return "\n".join(out)


def c_array_u8(name, data):
    items = [f"0x{b:02x}" for b in data]
    return (f"static const uint8_t {name}[] = {{\n"
            f"{_fmt_array_body(items)}\n"
            f"}};\n")


def c_array_u16(name, data):
    items = [str(v) for v in data]
    return (f"static const uint16_t {name}[] = {{\n"
            f"{_fmt_array_body(items)}\n"
            f"}};\n")


def emit_fonts():
    lines = []
    hdr = [
        "/* Generated by tools/gen_michi_ui_assets.py - DO NOT EDIT.",
        f" * Pillow {PIL_VER}; source fonts:",
        " *   SM : Google Sans Medium 11 px",
        " *   MD : Google Sans Regular 14 px",
        " *   LG : Google Sans Regular 20 px",
        " *   PIN: Google Sans Regular 36 px (charset: digits, space, '.')",
        " *   XS : hand-designed 8 px bitmap art (see generator script).",
        " * Bitmap layout: column-major; byte = column, bit n = row n (LSB = top).",
        " * Proportional fonts: per-glyph width + advance tables; offsets index the",
        " * flat bitmap. Full-index 95 is the '…' ellipsis glyph (UTF-8 E2 80 A6).",
        " * PIN uses pin_map to remap the full index space onto its small charset.",
        " */",
    ]
    lines.extend(h + "\n" for h in hdr)

    def emit_font(tag, glyphs, height, baseline):
        bpc = -(-height // 8)
        bitmap = bytearray()
        width = []
        advance = []
        offset = []
        for code in FULL_CHARS:
            cols, w, adv = glyphs[code]
            offset.append(len(bitmap))
            width.append(w)
            advance.append(adv)
            for col in cols:
                for b in range(bpc):
                    bitmap.append((col >> (8 * b)) & 0xFF)
        offset.append(len(bitmap))  # sentinel, unused
        lines.append(c_array_u8(f"michi_ui_{tag}_bitmap", bitmap))
        lines.append(c_array_u8(f"michi_ui_{tag}_width", width))
        lines.append(c_array_u8(f"michi_ui_{tag}_advance", advance))
        lines.append(c_array_u16(f"michi_ui_{tag}_offset", offset))
        lines.append(f"/* michi_ui_{tag}: height={height} baseline={baseline} "
                     f"bytes_per_col={-(-height // 8)} */\n")

    # XS
    xsh = 8
    glyphs = {}
    for c in FULL_CHARS:
        code = ord(c)
        if code == 0x2026:
            rows, w, adv = xs_glyph_art(0xE2_80_A6)
        else:
            rows, w, adv = xs_glyph_art(code)
        cols = []
        for x in range(w):
            col = 0
            for y in range(xsh):
                if rows[y][x] == "#":
                    col |= 1 << y
            cols.append(col)
        glyphs[c] = (cols, w, adv)
    emit_font("xs", glyphs, 8, 6)

    # SM / MD / LG
    for tag, (path, px) in PIL_FONTS.items():
        if tag == "pin":
            continue
        font = ImageFont.truetype(path, px)
        asc, desc = font.getmetrics()
        box_h = asc + desc
        glyphs = {ch: pil_glyph(font, ch, box_h) for ch in FULL_CHARS}
        emit_font(tag, glyphs, box_h, asc)

    # PIN (charset subset + pin_map)
    font = ImageFont.truetype(FONT_REGULAR, 36)
    asc, desc = font.getmetrics()
    box_h = asc  # digits have no descenders; '.' sits above the baseline
    pin_glyphs = {ch: pil_glyph(font, ch, box_h) for ch in PIN_CHARS}
    pin_index = {}
    bpc = -(-box_h // 8)
    bitmap = bytearray()
    width = []
    advance = []
    offset = []
    for ch in PIN_CHARS:
        cols, w, adv = pin_glyphs[ch]
        pin_index[ch] = len(width)
        offset.append(len(bitmap))
        width.append(w)
        advance.append(adv)
        for col in cols:
            for b in range(bpc):
                bitmap.append((col >> (8 * b)) & 0xFF)
    offset.append(len(bitmap))
    lines.append(c_array_u8("michi_ui_pin_bitmap", bitmap))
    lines.append(c_array_u8("michi_ui_pin_width", width))
    lines.append(c_array_u8("michi_ui_pin_advance", advance))
    lines.append(c_array_u16("michi_ui_pin_offset", offset))
    lines.append(f"/* michi_ui_pin: height={box_h} baseline={box_h} "
                 f"bytes_per_col={-(-box_h // 8)} */\n")

    # pin_map: full index -> pin glyph index
    pin_map = []
    for code in range(0x20, 0x7F):
        pin_map.append(pin_index.get(chr(code), pin_index[" "]))
    pin_map.append(pin_index[" "])  # ellipsis -> space (no glyph)
    lines.append(c_array_u8("michi_ui_pin_map", pin_map))
    lines.append("/* pin_map: full index -> PIN glyph (unsupported -> space). */\n")

    os.makedirs(os.path.dirname(FONTS_OUT), exist_ok=True)
    with open(FONTS_OUT, "w") as f:
        f.writelines(lines)
    print(f"wrote {FONTS_OUT}")


def emit_icons():
    lines = []
    lines.append("/* Generated by tools/gen_michi_ui_assets.py - DO NOT EDIT.\n")
    lines.append(" * 1 bpp, row-major, MSB first; first two bytes are width,height.\n")
    lines.append(" * Sizes: all icons at 12/20/32; the cat also at 24 and 48.\n */\n")
    for name, prims in ICONS:
        for size in ICON_SIZES[name]:
            rows = rasterize(prims, size, ICON_STROKE[size])
            data = pack_rows(rows)
            arr = [size, size] + list(data)
            lines.append(c_array_u8(f"michi_ui_icon_{name}_{size}", arr))
    os.makedirs(os.path.dirname(ICONS_OUT), exist_ok=True)
    with open(ICONS_OUT, "w") as f:
        f.writelines(lines)
    print(f"wrote {ICONS_OUT}")


def previews(out_dir):
    """ASCII-art previews to stdout (readable in terminal/CI)."""
    print("\n=== THEME (RGB888 -> RGB565, rounded) ===")
    for name, r, g, b in PALETTE:
        print(f"  {name:28s} #{r:02X}{g:02X}{b:02X} -> 0x{rgb565(r, g, b):04X}")

    print("\n=== XS font (charset) ===")
    for code in range(0x20, 0x7F):
        rows, w, adv = xs_glyph_art(code)
        ch = chr(code)
        print(f"{ch} (w={w},adv={adv})")
        for r in rows:
            print("  " + r)
        print()

    print("\n=== ICONS ===")
    for name, prims in ICONS:
        for size in ICON_SIZES[name]:
            rows = rasterize(prims, size, ICON_STROKE[size])
            print(f"--- {name} {size} ---")
            for r in art(rows):
                print("  " + r)


def main():
    if "--preview" in sys.argv:
        previews(sys.argv[sys.argv.index("--preview") + 1]
                 if len(sys.argv) > sys.argv.index("--preview") + 1 else "/tmp")
        return
    emit_fonts()
    emit_icons()


if __name__ == "__main__":
    main()
