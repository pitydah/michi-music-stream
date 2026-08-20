#!/usr/bin/env python3
"""Michi UI asset generator: bitmap fonts + icons + theme verification.

Generates (committed, build-time data only - no runtime dependency):
  assets/fonts/michi_ui_fonts_data.h
  assets/icons/michi_ui_icons_data.h

Design decisions:
  * Fonts are PROPORTIONAL (per-glyph advance table), 1-bit or 2-bit
    column-major bitmaps (LSB = top row), stored const in flash.
  * Open-source typography: Noto Sans or Inter (both SIL Open Font License).
  * XS (8 px em): pixel art / hinting.
  * SM (13 px): Medium weight (line height 16).
  * MD (17 px): Regular (line height 20).
  * LG (26 px): Regular (line height 30).
  * PIN (41 px): Regular (line height 46) - DIGITS + SPACE + '.' only.
  * Charset: 192 glyphs
    - 0..94: ASCII 0x20..0x7E (95 glyphs)
    - 95..190: Latin-1 Supplement 0x00A0..0x00FF (96 glyphs: á, é, í, ó, ú, ñ, ¿, ¡, etc.)
    - 191: Ellipsis '…' (UTF-8 E2 80 A6)
  * Icons: procedural vector strokes rasterized with 4x4 supersampling.
    Boutique audio aesthetic for Michi Cat and all symbols.

Usage:
  python3 tools/gen_michi_ui_assets.py [--preview DIR]
    [--font {notosans|inter}] [--raster {1bit|2bit}]

Requires: Pillow (dev-only; the generated headers are committed).
"""
import argparse
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

try:
    import PIL
    PIL_VER = PIL.__version__
except ImportError:
    PIL_VER = "?"

# ---------------------------------------------------------------------------
# Font path discovery: support both Noto Sans and Inter.
# ---------------------------------------------------------------------------

NOTO_DIR = "/usr/share/fonts/noto"
INTER_DIRS = [
    "/usr/share/fonts/inter",
    "/usr/share/fonts/truetype/inter",
    "/usr/local/share/fonts/inter",
    os.path.expanduser("~/.local/share/fonts/inter"),
]

NOTO_REGULAR = f"{NOTO_DIR}/NotoSans-Regular.ttf"
NOTO_MEDIUM  = f"{NOTO_DIR}/NotoSans-Medium.ttf"


def _find_inter(weight="Regular"):
    candidates = [f"Inter-{weight}.ttf", f"Inter_{weight}.ttf",
                  f"inter-{weight.lower()}.ttf", "Inter.ttf", "inter.ttf"]
    for d in INTER_DIRS:
        for name in candidates:
            p = os.path.join(d, name)
            if os.path.exists(p):
                return p
    return None


HERE = os.path.dirname(os.path.abspath(__file__))
FONTS_OUT = os.path.join(HERE, "..", "assets", "fonts", "michi_ui_fonts_data.h")
ICONS_OUT = os.path.join(HERE, "..", "assets", "icons", "michi_ui_icons_data.h")

# ---------------------------------------------------------------------------
# Theme palette (RGB888 sources) -> RGB565 with proper rounding.
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
        elif kind == "F":  # filled polygon
            polys.append(p[1])
        elif kind == "P":  # polygon outline
            poly = p[1]
            n = len(poly)
            for i in range(n):
                x0, y0 = poly[i]
                x1, y1 = poly[(i + 1) % n]
                segs.append((x0, y0, x1, y1))
                discs.append((x0, y0, 0.0))
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
    r = (stroke / 2.0) / size
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


def pack_rows(rows):
    """Row-major, 1 bpp, MSB first."""
    out = bytearray()
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
# Icon definitions (unit box, y down) — Boutique Hi-Fi Audio Aesthetic
# ---------------------------------------------------------------------------
CAT_BOUTIQUE = [
    # Head contour (smooth rounded face)
    ("C", 0.50, 0.56, 0.30),
    # Left outer ear
    ("L", 0.24, 0.38, 0.18, 0.08),
    ("L", 0.18, 0.08, 0.38, 0.26),
    # Right outer ear
    ("L", 0.76, 0.38, 0.82, 0.08),
    ("L", 0.82, 0.08, 0.62, 0.26),
    # Left inner ear detail
    ("L", 0.26, 0.32, 0.22, 0.14),
    ("L", 0.22, 0.14, 0.34, 0.26),
    # Right inner ear detail
    ("L", 0.74, 0.32, 0.78, 0.14),
    ("L", 0.78, 0.14, 0.66, 0.26),
    # Eyes
    ("D", 0.36, 0.50, 0.04),
    ("D", 0.64, 0.50, 0.04),
    # Nose
    ("D", 0.50, 0.60, 0.025),
    # Mouth
    ("L", 0.50, 0.60, 0.50, 0.67),
    ("A", 0.44, 0.67, 0.06, 0, 180),
    ("A", 0.56, 0.67, 0.06, 0, 180),
]

WIFI = [
    ("A", 0.50, 0.64, 0.50, 225, 315),
    ("A", 0.50, 0.64, 0.30, 225, 315),
    ("A", 0.50, 0.64, 0.10, 225, 315),
    ("D", 0.50, 0.88, 0.06)
]
SERVER = [
    ("R", 0.18, 0.12, 0.64, 0.76, 0.07),
    ("L", 0.26, 0.40, 0.74, 0.40),
    ("L", 0.26, 0.60, 0.74, 0.60),
    ("D", 0.32, 0.26, 0.04),
    ("D", 0.32, 0.50, 0.04),
    ("D", 0.32, 0.74, 0.04)
]
SPEAKER = [
    ("F", [(0.20, 0.36), (0.38, 0.36), (0.58, 0.18), (0.58, 0.82), (0.38, 0.64), (0.20, 0.64)]),
    ("A", 0.58, 0.50, 0.16, -45, 45),
    ("A", 0.58, 0.50, 0.28, -45, 45),
]
PLAY = [("F", [(0.30, 0.16), (0.30, 0.84), (0.78, 0.50)])]
PAUSE = [("Rf", 0.26, 0.16, 0.18, 0.68, 0.04),
         ("Rf", 0.56, 0.16, 0.18, 0.68, 0.04)]
PAIR = [("C", 0.38, 0.42, 0.24), ("C", 0.62, 0.58, 0.24)]
BUTTON = [("C", 0.50, 0.50, 0.32), ("C", 0.50, 0.50, 0.16)]
WARNING = [("P", [(0.50, 0.08), (0.92, 0.84), (0.08, 0.84)]),
           ("L", 0.50, 0.38, 0.50, 0.60),
           ("D", 0.50, 0.72, 0.045)]
ERROR = [("C", 0.50, 0.50, 0.32),
         ("L", 0.32, 0.32, 0.68, 0.68),
         ("L", 0.68, 0.32, 0.32, 0.68)]
UPDATE = [("L", 0.16, 0.80, 0.84, 0.80),
          ("L", 0.50, 0.16, 0.50, 0.64),
          ("L", 0.32, 0.46, 0.50, 0.64),
          ("L", 0.68, 0.46, 0.50, 0.64)]
WAVE = [("A", 0.32, 0.50, 0.16, 0, 180),
        ("A", 0.64, 0.50, 0.16, 180, 360)]

ICONS = [
    ("cat", CAT_BOUTIQUE),
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

ICON_SIZES = {name: [12, 16, 20, 24, 32] for name, _ in ICONS}
ICON_SIZES["cat"] = [12, 16, 20, 24, 32, 48, 56]
ICON_SIZES["speaker"] = [12, 16, 20, 24, 32]
ICON_SIZES["play"] = [12, 16, 20, 24, 32, 48]
ICON_SIZES["pause"] = [12, 16, 20, 24, 32]

ICON_STROKE = {12: 1.0, 16: 1.3, 20: 1.6, 24: 1.9, 32: 2.5, 48: 3.6, 56: 4.2}

# ---------------------------------------------------------------------------
# Typography Definitions & Charset (192 Glyphs)
# ---------------------------------------------------------------------------

# Charset: 0x20..0x7E (95 chars) + 0x00A0..0x00FF (96 chars) + U+2026 (1 char)
FULL_CHARS = [chr(c) for c in range(0x20, 0x7F)] + \
             [chr(c) for c in range(0x00A0, 0x0100)] + \
             ["\u2026"]
assert len(FULL_CHARS) == 192, f"FULL_CHARS has {len(FULL_CHARS)} characters (want 192)"

PIN_CHARS = "0123456789 ."

# (weight_key, pt, height, line_height, raster_bits)
FONT_SPECS = {
    "xs":  ("medium",  6, 8,  10, 1),
    "sm":  ("medium",  9, 13, 16, 1),
    "md":  ("regular",12, 17, 20, 1),
    "lg":  ("medium", 18, 26, 30, 1),
    "pin": ("regular",38, 41, 46, 1),
}


def pil_glyph_1bit(font, ch, box_h, asc_align=True):
    """Render one glyph into the shared em box."""
    tmp = Image.new("L", (800, box_h + 8))
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


def pil_glyph_2bit(font, ch, box_h):
    tmp = Image.new("L", (800, box_h + 8))
    d = ImageDraw.Draw(tmp)
    d.text((0, 0), ch, fill=255, font=font)
    bbox = d.textbbox((0, 0), ch, font=font)
    adv = max(1, int(round(d.textlength(ch, font=font))))
    if bbox is None or bbox[2] <= bbox[0]:
        return [], 0, adv
    w = bbox[2] - bbox[0]
    glyph = tmp.crop((bbox[0], 0, bbox[2], box_h))
    cols = []
    for x in range(w):
        col = 0
        for y in range(box_h):
            pix = glyph.getpixel((x, y))
            level = min(3, pix >> 6)
            col |= level << (2 * y)
        cols.append(col)
    return cols, w, adv

def emit_fonts(font_family="notosans", raster_bits_override=1):
    if font_family == "inter":
        inter_reg = _find_inter("Regular")
        inter_med = _find_inter("Medium")
        if inter_reg is None:
            print("WARNING: Inter Regular not found; falling back to Noto Sans Regular.", file=sys.stderr)
            inter_reg = NOTO_REGULAR
        if inter_med is None:
            print("WARNING: Inter Medium not found; falling back to Noto Sans Medium.", file=sys.stderr)
            inter_med = NOTO_MEDIUM
        font_regular = inter_reg
        font_medium  = inter_med
        family_label = "Inter"
    else:
        font_regular = NOTO_REGULAR
        font_medium  = NOTO_MEDIUM
        family_label = "Noto Sans"

    paths = {'regular': font_regular, 'medium': font_medium}
    raster_label = f"{raster_bits_override}-bit"

    lines = []
    hdr = [
        "/* Generated by tools/gen_michi_ui_assets.py - DO NOT EDIT.",
        f" * Pillow {PIL_VER}; source font: {family_label} (SIL Open Font License)",
        f" * Raster mode override: {raster_label}",
        " *   XS : 8 px em (line height 10)",
        f" *   SM : {family_label} Medium 13 px em (line height 16)",
        f" *   MD : {family_label} Regular 17 px em (line height 20)",
        f" *   LG : {family_label} Regular 26 px em (line height 30)",
        f" *   PIN: {family_label} Regular 41 px em (line height 46, charset: digits, space, '.')",
        " * Charset (192 glyphs):",
        " *   0..94: ASCII 0x20..0x7E (95 glyphs)",
        " *   95..190: Latin-1 Supplement 0x00A0..0x00FF (96 glyphs: á, é, í, ó, ú, ñ, ¿, ¡, etc.)",
        " *   191: Ellipsis '…' (U+2026)",
        " * Bitmap layout: column-major; byte = column, bit n = row n (LSB = top).",
        " */",
    ]
    lines.extend(h + "\n" for h in hdr)

    def emit_font(tag, glyphs, height, baseline, line_h, raster_bits):
        if raster_bits == 2:
            bpc = -(- (height * 2) // 8)
        else:
            bpc = -(-height // 8)
        bitmap = bytearray()
        width = []
        advance = []
        offset = []
        for ch in FULL_CHARS:
            cols, w, adv = glyphs.get(ch, ([], 0, 4))
            offset.append(len(bitmap))
            width.append(w)
            advance.append(adv)
            for col in cols:
                for b in range(bpc):
                    bitmap.append((col >> (8 * b)) & 0xFF)
        offset.append(len(bitmap))
        lines.append(c_array_u8(f"michi_ui_{tag}_bitmap", bitmap))
        lines.append(c_array_u8(f"michi_ui_{tag}_width", width))
        lines.append(c_array_u8(f"michi_ui_{tag}_advance", advance))
        lines.append(c_array_u16(f"michi_ui_{tag}_offset", offset))
        lines.append(f"/* michi_ui_{tag}: height={height} baseline={baseline} line_height={line_h} "
                     f"bytes_per_col={bpc} raster={raster_bits}-bit */\n")

    for tag, (weight, pt, box_h, line_h, base_raster) in FONT_SPECS.items():
        raster = base_raster
        if raster_bits_override == 2 and tag in ("sm", "md", "lg"):
            raster = 2

        path = paths[weight]
        font = ImageFont.truetype(path, pt)
        asc, desc = font.getmetrics()

        if raster == 2:
            glyphs = {ch: pil_glyph_2bit(font, ch, box_h) for ch in FULL_CHARS}
        else:
            glyphs = {ch: pil_glyph_1bit(font, ch, box_h) for ch in FULL_CHARS}
        
        if tag == "pin":
            pin_map = []
            for ch in FULL_CHARS:
                if ch in PIN_CHARS:
                    pin_map.append(FULL_CHARS.index(ch))
                elif ch == "\u2026":
                    pin_map.append(FULL_CHARS.index("."))
                else:
                    pin_map.append(FULL_CHARS.index(" "))
            lines.append(c_array_u8("michi_ui_pin_map", pin_map))
            lines.append("/* pin_map: full 192 index -> PIN glyph (unsupported -> space, '…' -> '.'). */\n")

        emit_font(tag, glyphs, box_h, asc, line_h, raster)

    os.makedirs(os.path.dirname(FONTS_OUT), exist_ok=True)
    with open(FONTS_OUT, "w") as f:
        f.writelines(lines)
    print(f"wrote {FONTS_OUT} (font={family_label}, raster override={raster_label})")

def emit_icons():
    lines = []
    lines.append("/* Generated by tools/gen_michi_ui_assets.py - DO NOT EDIT.\n")
    lines.append(" * 1 bpp, row-major, MSB first; first two bytes are width,height.\n */\n")
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


def main():
    parser = argparse.ArgumentParser(
        description="Michi UI asset generator – fonts + icons + theme verification.")
    parser.add_argument(
        "--preview", metavar="DIR", default=None,
        help="Write per-glyph PNG previews to DIR (dev only).")
    parser.add_argument(
        "--font", choices=["notosans", "inter"], default="notosans",
        help="Font family to rasterize (default: notosans).")
    parser.add_argument(
        "--raster", choices=["1bit", "2bit"], default="1bit",
        help="Bitmap depth: 1bit = on/off, 2bit = 4-level AA (default: 1bit).")
    args = parser.parse_args()

    raster_bits = 2 if args.raster == "2bit" else 1
    emit_fonts(font_family=args.font, raster_bits_override=raster_bits)
    emit_icons()

if __name__ == "__main__":
    main()
