#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate Nintendo 3DS CIA/SMDH artwork and startup banner audio for PSX3DS.

Uses only the Python 3 standard library (math, struct, wave, zlib) so release
packaging and CI remain 100% reproducible without external imaging libraries.

Outputs:
  - assets/3ds/icon.png    (48x48 RGBA8 Home Menu / Homebrew Launcher icon)
  - assets/3ds/banner.png  (256x128 RGBA8 transparent 3DS Home Menu banner)
  - assets/3ds/banner.wav  (1.25s 32 kHz 16-bit stereo warm chord chime < 2s)
"""

from __future__ import annotations

import math
import os
import struct
import wave
import zlib
from pathlib import Path


# ---------------------------------------------------------------------------
# Minimal PNG writer (RGBA8, standard library only)
# ---------------------------------------------------------------------------
def write_png_rgba(path: Path, width: int, height: int, rgba: bytearray) -> None:
    assert len(rgba) == width * height * 4
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)  # Filter type 0 (None)
        row_off = y * stride
        raw.extend(rgba[row_off : row_off + stride])

    def chunk(tag: bytes, payload: bytes) -> bytes:
        crc = zlib.crc32(tag)
        crc = zlib.crc32(payload, crc) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", crc)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    idat = zlib.compress(bytes(raw), level=9)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


# ---------------------------------------------------------------------------
# Anti-aliased 2D SDF drawing helpers
# ---------------------------------------------------------------------------
def clamp01(x: float) -> float:
    if x <= 0.0:
        return 0.0
    if x >= 1.0:
        return 1.0
    return x


def smooth_cov(sd: float, aa: float = 0.85) -> float:
    return clamp01(0.5 - sd / aa)


def sd_round_rect(px: float, py: float, cx: float, cy: float, hw: float, hh: float, r: float) -> float:
    qx = abs(px - cx) - (hw - r)
    qy = abs(py - cy) - (hh - r)
    out = math.hypot(max(qx, 0.0), max(qy, 0.0))
    inside = min(max(qx, qy), 0.0)
    return out + inside - r


def sd_segment(px: float, py: float, ax: float, ay: float, bx: float, by: float) -> float:
    vx, vy = bx - ax, by - ay
    wx, wy = px - ax, py - ay
    denom = vx * vx + vy * vy
    t = 0.0 if denom <= 1e-9 else clamp01((wx * vx + wy * vy) / denom)
    dx = wx - t * vx
    dy = wy - t * vy
    return math.hypot(dx, dy)


def sd_equilateral_triangle_ring(px: float, py: float, cx: float, cy: float, radius: float) -> float:
    pts = []
    for k in range(3):
        ang = -math.pi / 2.0 + k * (2.0 * math.pi / 3.0)
        pts.append((cx + radius * math.cos(ang), cy + radius * 0.12 + radius * math.sin(ang)))
    return min(
        sd_segment(px, py, pts[0][0], pts[0][1], pts[1][0], pts[1][1]),
        sd_segment(px, py, pts[1][0], pts[1][1], pts[2][0], pts[2][1]),
        sd_segment(px, py, pts[2][0], pts[2][1], pts[0][0], pts[0][1]),
    )


def sd_square_ring(px: float, py: float, cx: float, cy: float, half: float) -> float:
    dx = abs(px - cx) - half
    dy = abs(py - cy) - half
    return abs(math.hypot(max(dx, 0.0), max(dy, 0.0)) + min(max(dx, dy), 0.0))


def sd_cross(px: float, py: float, cx: float, cy: float, arm: float) -> float:
    return min(
        sd_segment(px, py, cx - arm, cy - arm, cx + arm, cy + arm),
        sd_segment(px, py, cx - arm, cy + arm, cx + arm, cy - arm),
    )


def blend_over(
    dst: tuple[float, float, float, float],
    src_rgb: tuple[float, float, float],
    src_a: float,
) -> tuple[float, float, float, float]:
    sa = clamp01(src_a)
    if sa <= 0.0:
        return dst
    dr, dg, db, da = dst
    out_a = sa + da * (1.0 - sa)
    if out_a <= 1e-7:
        return (0.0, 0.0, 0.0, 0.0)
    inv = 1.0 - sa
    out_r = (src_rgb[0] * sa + dr * da * inv) / out_a
    out_g = (src_rgb[1] * sa + dg * da * inv) / out_a
    out_b = (src_rgb[2] * sa + db * da * inv) / out_a
    return (out_r, out_g, out_b, out_a)


# ---------------------------------------------------------------------------
# Crisp vector stroke glyphs for banner lettering ("PSX3DS") & 5x7 pixel font
# ---------------------------------------------------------------------------
VECTOR_GLYPHS: dict[str, tuple[float, list[tuple[float, float, float, float]]]] = {
    "P": (
        0.72,
        [
            (0.08, 0.00, 0.08, 1.00),
            (0.08, 0.00, 0.50, 0.00),
            (0.50, 0.00, 0.64, 0.12),
            (0.64, 0.12, 0.64, 0.38),
            (0.64, 0.38, 0.50, 0.50),
            (0.50, 0.50, 0.08, 0.50),
        ],
    ),
    "S": (
        0.70,
        [
            (0.60, 0.12, 0.48, 0.00),
            (0.48, 0.00, 0.18, 0.00),
            (0.18, 0.00, 0.06, 0.13),
            (0.06, 0.13, 0.06, 0.34),
            (0.06, 0.34, 0.18, 0.47),
            (0.18, 0.47, 0.50, 0.52),
            (0.50, 0.52, 0.62, 0.65),
            (0.62, 0.65, 0.62, 0.87),
            (0.62, 0.87, 0.48, 1.00),
            (0.48, 1.00, 0.16, 1.00),
            (0.16, 1.00, 0.05, 0.88),
        ],
    ),
    "X": (
        0.74,
        [
            (0.06, 0.00, 0.66, 1.00),
            (0.66, 0.00, 0.06, 1.00),
        ],
    ),
    "3": (
        0.70,
        [
            (0.08, 0.10, 0.20, 0.00),
            (0.20, 0.00, 0.50, 0.00),
            (0.50, 0.00, 0.62, 0.13),
            (0.62, 0.13, 0.62, 0.36),
            (0.62, 0.36, 0.48, 0.48),
            (0.26, 0.48, 0.50, 0.48),
            (0.50, 0.48, 0.64, 0.62),
            (0.64, 0.62, 0.64, 0.87),
            (0.64, 0.87, 0.50, 1.00),
            (0.50, 1.00, 0.18, 1.00),
            (0.18, 1.00, 0.06, 0.88),
        ],
    ),
    "D": (
        0.74,
        [
            (0.08, 0.00, 0.08, 1.00),
            (0.08, 0.00, 0.44, 0.00),
            (0.44, 0.00, 0.64, 0.20),
            (0.64, 0.20, 0.64, 0.80),
            (0.64, 0.80, 0.44, 1.00),
            (0.44, 1.00, 0.08, 1.00),
        ],
    ),
}

FONT_5X7: dict[str, list[str]] = {
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "C": ["01110", "10001", "10000", "10000", "10000", "10001", "01110"],
    "D": ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "G": ["01110", "10001", "10000", "10111", "10001", "10001", "01110"],
    "H": ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
    "I": ["01110", "00100", "00100", "00100", "00100", "00100", "01110"],
    "J": ["00111", "00010", "00010", "00010", "10010", "10010", "01100"],
    "L": ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    "M": ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    "N": ["10001", "11001", "10101", "10011", "10001", "10001", "10001"],
    "O": ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    "P": ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    "R": ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    "T": ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    "U": ["10001", "10001", "10001", "10001", "10001", "10001", "01110"],
    "V": ["10001", "10001", "10001", "10001", "10001", "01010", "00100"],
    "Y": ["10001", "10001", "01010", "00100", "00100", "00100", "00100"],
    "0": ["01110", "10011", "10101", "10101", "11001", "10001", "01110"],
    "1": ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    "2": ["01110", "10001", "00001", "00110", "01000", "10000", "11111"],
    ".": ["00000", "00000", "00000", "00000", "00000", "01100", "01100"],
    "-": ["00000", "00000", "00000", "11111", "00000", "00000", "00000"],
    "/": ["00001", "00010", "00010", "00100", "01000", "01000", "10000"],
    " ": ["00000", "00000", "00000", "00000", "00000", "00000", "00000"],
}


def stamp_5x7(
    rgba: bytearray,
    width: int,
    height: int,
    x0: int,
    y0: int,
    text: str,
    color: tuple[int, int, int],
    alpha: float = 1.0,
    spacing: int = 6,
) -> None:
    cx = x0
    for ch in text:
        rows = FONT_5X7.get(ch, FONT_5X7[" "])
        for ry, row in enumerate(rows):
            py = y0 + ry
            if py < 0 or py >= height:
                continue
            for rx, bit in enumerate(row):
                if bit != "1":
                    continue
                px = cx + rx
                if px < 0 or px >= width:
                    continue
                off = (py * width + px) * 4
                dst = (
                    rgba[off + 0] / 255.0,
                    rgba[off + 1] / 255.0,
                    rgba[off + 2] / 255.0,
                    rgba[off + 3] / 255.0,
                )
                src_rgb = (color[0] / 255.0, color[1] / 255.0, color[2] / 255.0)
                out = blend_over(dst, src_rgb, alpha)
                rgba[off + 0] = int(round(clamp01(out[0]) * 255.0))
                rgba[off + 1] = int(round(clamp01(out[1]) * 255.0))
                rgba[off + 2] = int(round(clamp01(out[2]) * 255.0))
                rgba[off + 3] = int(round(clamp01(out[3]) * 255.0))
        cx += spacing


# ---------------------------------------------------------------------------
# 1. Generate 48x48 Home Menu / Homebrew Launcher Icon (assets/3ds/icon.png)
# ---------------------------------------------------------------------------
def generate_icon(path: Path) -> None:
    w, h = 48, 48
    rgba = bytearray(w * h * 4)

    # 2x2 rotated-grid supersampling for crisp edges even at 24x24 SMDH thumbnail
    sub_offsets = ((0.25, 0.25), (0.75, 0.25), (0.25, 0.75), (0.75, 0.75))

    col_tri = (0.18, 0.92, 0.64)     # Mint/emerald triangle (top)
    col_cir = (1.00, 0.34, 0.42)     # Coral-red circle (right)
    col_crs = (0.32, 0.60, 1.00)     # Cobalt-blue cross (bottom)
    col_sqr = (0.95, 0.44, 0.76)     # Orchid-pink square (left)

    for y in range(h):
        for x in range(w):
            acc_r = acc_g = acc_b = acc_a = 0.0
            for ox, oy in sub_offsets:
                px = x + ox
                py = y + oy
                col = (0.0, 0.0, 0.0, 0.0)

                # Rounded icon base (full 48x48 coverage with neat 7px corner radius;
                # fill crisp dark slate on outer corners too so 3DS RGB565 icon tiles have no harsh halo)
                col = (0.05, 0.07, 0.13, 1.0)

                d_card = sd_round_rect(px, py, 24.0, 24.0, 22.5, 22.5, 6.5)
                if d_card < 1.0:
                    gy = clamp01(py / 48.0)
                    # Deep twilight slate gradient + radial center glow
                    rad = math.hypot(px - 24.0, py - 21.0) / 26.0
                    glow = max(0.0, 1.0 - rad) ** 1.8
                    bg = (
                        0.08 + 0.05 * (1.0 - gy) + 0.07 * glow,
                        0.11 + 0.07 * (1.0 - gy) + 0.10 * glow,
                        0.21 + 0.10 * (1.0 - gy) + 0.18 * glow,
                    )
                    col = blend_over(col, bg, smooth_cov(d_card, 0.75))

                    # Crisp subtle metallic rim highlight
                    rim_sd = abs(d_card + 0.9) - 0.55
                    if rim_sd < 0.8:
                        rim_col = (0.36, 0.52, 0.82) if py < 24.0 else (0.18, 0.26, 0.46)
                        col = blend_over(col, rim_col, 0.65 * smooth_cov(rim_sd, 0.75))

                # Subtle retro diagonal color ribbon accent along bottom bar
                if py > 39.0 and abs(px - 24.0) < 18.0:
                    bar_sd = sd_round_rect(px, py, 24.0, 42.2, 15.5, 1.35, 1.2)
                    if bar_sd < 0.8:
                        t = clamp01((px - 8.5) / 31.0)
                        if t < 0.33:
                            u = t / 0.33
                            rc = (1.0 - 0.0 * u, 0.24 + 0.54 * u, 0.24 - 0.10 * u)
                        elif t < 0.66:
                            u = (t - 0.33) / 0.33
                            rc = (1.0 - 0.80 * u, 0.78 + 0.12 * u, 0.14 + 0.50 * u)
                        else:
                            u = (t - 0.66) / 0.34
                            rc = (0.20 + 0.14 * u, 0.90 - 0.32 * u, 0.64 + 0.36 * u)
                        col = blend_over(col, rc, 0.92 * smooth_cov(bar_sd, 0.7))

                # Center subtle diamond reticle connecting the 4 face symbols
                d_ret = abs(abs(px - 24.0) + abs(py - 21.5) - 12.2) - 0.35
                if d_ret < 0.8:
                    col = blend_over(col, (0.32, 0.44, 0.68), 0.22 * smooth_cov(d_ret, 0.7))

                # 1) Top: Green Triangle (cx=24, cy=10.2)
                d_tri = sd_equilateral_triangle_ring(px, py, 24.0, 10.2, 5.0) - 1.15
                if d_tri < 2.2:
                    col = blend_over(col, col_tri, 0.22 * clamp01(1.0 - max(d_tri, 0.0) / 2.2))
                if d_tri < 0.8:
                    col = blend_over(col, col_tri, smooth_cov(d_tri, 0.7))

                # 2) Right: Coral-Red Circle (cx=35.5, cy=21.5)
                d_cir = abs(math.hypot(px - 35.5, py - 21.5) - 4.4) - 1.15
                if d_cir < 2.2:
                    col = blend_over(col, col_cir, 0.22 * clamp01(1.0 - max(d_cir, 0.0) / 2.2))
                if d_cir < 0.8:
                    col = blend_over(col, col_cir, smooth_cov(d_cir, 0.7))

                # 3) Bottom: Cobalt-Blue Cross (cx=24.0, cy=32.6)
                d_crs = sd_cross(px, py, 24.0, 32.6, 4.1) - 1.20
                if d_crs < 2.2:
                    col = blend_over(col, col_crs, 0.22 * clamp01(1.0 - max(d_crs, 0.0) / 2.2))
                if d_crs < 0.8:
                    col = blend_over(col, col_crs, smooth_cov(d_crs, 0.7))

                # 4) Left: Orchid-Pink Square (cx=12.5, cy=21.5)
                d_sqr = sd_square_ring(px, py, 12.5, 21.5, 4.1) - 1.15
                if d_sqr < 2.2:
                    col = blend_over(col, col_sqr, 0.22 * clamp01(1.0 - max(d_sqr, 0.0) / 2.2))
                if d_sqr < 0.8:
                    col = blend_over(col, col_sqr, smooth_cov(d_sqr, 0.7))

                acc_r += col[0]
                acc_g += col[1]
                acc_b += col[2]
                acc_a += col[3]

            inv_n = 1.0 / len(sub_offsets)
            off = (y * w + x) * 4
            rgba[off + 0] = int(round(clamp01(acc_r * inv_n) * 255.0))
            rgba[off + 1] = int(round(clamp01(acc_g * inv_n) * 255.0))
            rgba[off + 2] = int(round(clamp01(acc_b * inv_n) * 255.0))
            rgba[off + 3] = int(round(clamp01(acc_a * inv_n) * 255.0))

    write_png_rgba(path, w, h, rgba)


# ---------------------------------------------------------------------------
# 2. Generate 256x128 Home Menu Banner (assets/3ds/banner.png)
# ---------------------------------------------------------------------------
def point_in_poly(px: float, py: float, poly: list[tuple[float, float]]) -> bool:
    inside = False
    j = len(poly) - 1
    for i in range(len(poly)):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if ((yi > py) != (yj > py)) and (px < (xj - xi) * (py - yi) / (yj - yi + 1e-12) + xi):
            inside = not inside
        j = i
    return inside


def sd_poly(px: float, py: float, poly: list[tuple[float, float]]) -> float:
    min_d = min(
        sd_segment(px, py, poly[i][0], poly[i][1], poly[(i + 1) % len(poly)][0], poly[(i + 1) % len(poly)][1])
        for i in range(len(poly))
    )
    return -min_d if point_in_poly(px, py, poly) else min_d


def build_title_segments(
    text: str, x0: float, y0: float, gh: float, gw_scale: float, tracking: float
) -> list[tuple[str, int, float, float, float, float]]:
    segs: list[tuple[str, int, float, float, float, float]] = []
    cx = x0
    for idx, ch in enumerate(text):
        adv, strokes = VECTOR_GLYPHS[ch]
        for ax, ay, bx, by in strokes:
            segs.append(
                (
                    ch,
                    idx,
                    cx + ax * gw_scale,
                    y0 + ay * gh,
                    cx + bx * gw_scale,
                    y0 + by * gh,
                )
            )
        cx += adv * gw_scale + tracking
    return segs


def generate_banner(path: Path) -> None:
    w, h = 256, 128
    rgba = bytearray(w * h * 4)

    # Isometric 3D GTE Cube emblem centered at (48, 61)
    cube_cx, cube_cy, cube_r = 46.0, 61.0, 26.0
    p_center = (cube_cx, cube_cy)
    p_top = (cube_cx, cube_cy - cube_r)
    p_tr = (cube_cx + cube_r * 0.866, cube_cy - cube_r * 0.5)
    p_br = (cube_cx + cube_r * 0.866, cube_cy + cube_r * 0.5)
    p_bot = (cube_cx, cube_cy + cube_r)
    p_bl = (cube_cx - cube_r * 0.866, cube_cy + cube_r * 0.5)
    p_tl = (cube_cx - cube_r * 0.866, cube_cy - cube_r * 0.5)

    face_top = [p_top, p_tr, p_center, p_tl]
    face_left = [p_tl, p_center, p_bot, p_bl]
    face_right = [p_center, p_tr, p_br, p_bot]

    cube_edges = [
        (p_top, p_tr),
        (p_tr, p_br),
        (p_br, p_bot),
        (p_bot, p_bl),
        (p_bl, p_tl),
        (p_tl, p_top),
        (p_tl, p_center),
        (p_tr, p_center),
        (p_bot, p_center),
    ]

    title_segs = build_title_segments("PSX3DS", x0=84.0, y0=32.0, gh=34.0, gw_scale=29.0, tracking=3.5)

    for y in range(h):
        for x in range(w):
            px = x + 0.5
            py = y + 0.5
            col = (0.0, 0.0, 0.0, 0.0)

            # Soft outer drop shadow (floating card on 3DS Home Menu)
            d_shad = sd_round_rect(px, py - 2.5, 128.0, 64.0, 118.0, 52.0, 14.0)
            if d_shad < 5.0:
                shad_a = 0.42 * clamp01(1.0 - max(d_shad, 0.0) / 5.0) ** 1.6
                col = blend_over(col, (0.02, 0.03, 0.07), shad_a)

            # Main floating banner card (236x104, transparent outside!)
            d_card = sd_round_rect(px, py, 128.0, 63.0, 118.0, 52.0, 14.0)
            if d_card < 1.0:
                cov = smooth_cov(d_card, 1.0)
                gx = clamp01((px - 10.0) / 236.0)
                gy = clamp01((py - 11.0) / 104.0)

                # Rich deep midnight -> indigo gradient + left cube spotlight
                spot = max(0.0, 1.0 - math.hypot(px - cube_cx, py - cube_cy) / 62.0)
                bg_r = 0.06 + 0.05 * (1.0 - gy) + 0.08 * spot
                bg_g = 0.09 + 0.07 * (1.0 - gy) + 0.12 * spot
                bg_b = 0.18 + 0.12 * (1.0 - gx * 0.5) + 0.22 * spot

                # Subtle perspective retro horizon grid in lower third of card
                if py > 80.0:
                    grid_t = (py - 80.0) / 35.0
                    horiz = abs(math.sin(math.sqrt(max(grid_t, 0.02)) * 14.0))
                    persp = abs(math.sin(((px - 128.0) / (0.35 + grid_t * 1.3)) * 0.11))
                    line = max(clamp01(1.0 - horiz * 9.0), clamp01(1.0 - persp * 11.0))
                    bg_r += 0.05 * line * grid_t
                    bg_g += 0.11 * line * grid_t
                    bg_b += 0.20 * line * grid_t

                col = blend_over(col, (bg_r, bg_g, bg_b), cov)

                # Retro 4-color accent stripe near bottom inside card
                if 104.0 <= py <= 110.0 and 84.0 <= px <= 232.0:
                    d_bar = sd_round_rect(px, py, 158.0, 106.8, 73.0, 1.4, 1.3)
                    if d_bar < 1.0:
                        bt = (px - 85.0) / 146.0
                        if bt < 0.25:
                            bc = (0.96, 0.24, 0.30)  # Classic scarlet
                        elif bt < 0.50:
                            bc = (0.98, 0.76, 0.16)  # Warm gold
                        elif bt < 0.75:
                            bc = (0.18, 0.88, 0.62)  # Emerald green
                        else:
                            bc = (0.28, 0.58, 1.00)  # PlayStation cobalt
                        col = blend_over(col, bc, cov * smooth_cov(d_bar, 0.9))

                # Outer glass bevel border
                d_rim = abs(d_card + 1.1) - 0.65
                if d_rim < 1.0:
                    rim_c = (0.38, 0.56, 0.88) if py < 58.0 else (0.20, 0.30, 0.54)
                    col = blend_over(col, rim_c, 0.75 * cov * smooth_cov(d_rim, 0.9))

            # Isometric 3D GTE Cube on the left (bounding-box culled for speed)
            if 14.0 <= px <= 78.0 and 28.0 <= py <= 94.0:
                dt = sd_poly(px, py, face_top)
                if dt < 0.9:
                    col = blend_over(col, (0.20, 0.82, 0.66), 0.92 * smooth_cov(dt, 0.9))
                dl = sd_poly(px, py, face_left)
                if dl < 0.9:
                    col = blend_over(col, (0.24, 0.50, 0.95), 0.92 * smooth_cov(dl, 0.9))
                dr = sd_poly(px, py, face_right)
                if dr < 0.9:
                    col = blend_over(col, (0.92, 0.26, 0.38), 0.90 * smooth_cov(dr, 0.9))

                # Crisp wireframe highlight along cube edges
                d_edge = min(sd_segment(px, py, a[0], a[1], b[0], b[1]) for a, b in cube_edges) - 0.65
                if d_edge < 0.9:
                    col = blend_over(col, (0.92, 0.97, 1.00), 0.88 * smooth_cov(d_edge, 0.85))

            # Vector title "PSX3DS" (drop shadow + sleek bevel stroke)
            if 78.0 <= px <= 242.0 and 25.0 <= py <= 74.0:
                # Shadow pass
                min_sh = 99.0
                for _ch, _idx, ax, ay, bx, by in title_segs:
                    d = sd_segment(px, py - 2.0, ax + 1.5, ay, bx + 1.5, by) - 2.85
                    if d < min_sh:
                        min_sh = d
                if min_sh < 1.8:
                    col = blend_over(col, (0.01, 0.02, 0.06), 0.65 * clamp01(1.0 - max(min_sh, 0.0) / 1.8))

                # Foreground stroke pass ("PSX" platinum-ice, "3" Nintendo crimson, "DS" cool silver-blue)
                best_psx = 99.0
                best_3 = 99.0
                best_ds = 99.0
                for _ch, idx, ax, ay, bx, by in title_segs:
                    d = sd_segment(px, py, ax, ay, bx, by) - 2.65
                    if idx < 3:
                        if d < best_psx:
                            best_psx = d
                    elif idx == 3:
                        if d < best_3:
                            best_3 = d
                    else:
                        if d < best_ds:
                            best_ds = d

                shine = clamp01(1.0 - (py - 32.0) / 34.0)
                if best_psx < 1.0:
                    c_psx = (0.88 + 0.12 * shine, 0.92 + 0.08 * shine, 1.00)
                    col = blend_over(col, c_psx, smooth_cov(best_psx, 0.9))
                if best_ds < 1.0:
                    c_ds = (0.72 + 0.20 * shine, 0.82 + 0.14 * shine, 0.98)
                    col = blend_over(col, c_ds, smooth_cov(best_ds, 0.9))
                if best_3 < 1.0:
                    c_3 = (1.00, 0.20 + 0.22 * shine, 0.28 + 0.14 * shine)
                    col = blend_over(col, c_3, smooth_cov(best_3, 0.9))

            off = (y * w + x) * 4
            rgba[off + 0] = int(round(clamp01(col[0]) * 255.0))
            rgba[off + 1] = int(round(clamp01(col[1]) * 255.0))
            rgba[off + 2] = int(round(clamp01(col[2]) * 255.0))
            rgba[off + 3] = int(round(clamp01(col[3]) * 255.0))

    # Crisp 5x7 pixel typography subtitles aligned under "PSX3DS"
    stamp_5x7(rgba, w, h, 86, 77, "PLAYSTATION 1 EMULATOR", (222, 234, 255), alpha=0.95, spacing=6)
    stamp_5x7(rgba, w, h, 86, 90, "ARM11 JIT - PICA200 - CHD", (132, 188, 255), alpha=0.88, spacing=6)

    write_png_rgba(path, w, h, rgba)


# ---------------------------------------------------------------------------
# 3. Generate 1.25s Warm Retro Startup Chime (assets/3ds/banner.wav, < 2.0s)
# ---------------------------------------------------------------------------
def generate_chime_wav(path: Path) -> None:
    sample_rate = 32000
    duration_s = 1.25  # Strictly < 3DS 3.0s/2.0s Home Menu BCWAV banner limit
    n_frames = int(sample_rate * duration_s)

    # Lush rising Dmaj9 glassy fifths/bell bloom (original synthesized waveform):
    # D4 (293.66 Hz), A4 (440.00 Hz), E5 (659.25 Hz), F#5 (739.99 Hz), A5 (880.00 Hz)
    voices = [
        # (freq_hz, onset_s, amp, pan [-1..+1], decay_rate)
        (293.665, 0.00, 0.26, -0.15, 2.0),
        (440.000, 0.04, 0.23,  0.20, 2.2),
        (659.255, 0.09, 0.20, -0.35, 2.6),
        (739.989, 0.14, 0.18,  0.38, 2.9),
        (880.000, 0.20, 0.16,  0.00, 3.2),
    ]

    pcm = bytearray(n_frames * 4)
    two_pi = 2.0 * math.pi

    for i in range(n_frames):
        t = i / sample_rate
        left = 0.0
        right = 0.0

        # Master smooth fade-in (8 ms) and cosine fade-out over final 260 ms -> exact zero
        if t < 0.008:
            master = 0.5 * (1.0 - math.cos(math.pi * t / 0.008))
        elif t > duration_s - 0.26:
            rem = clamp01((duration_s - t) / 0.26)
            master = 0.5 * (1.0 - math.cos(math.pi * rem))
        else:
            master = 1.0

        for freq, onset, amp, pan, decay in voices:
            dt = t - onset
            if dt <= 0.0:
                continue
            attack = 1.0 - math.exp(-dt / 0.012)
            env = attack * math.exp(-decay * dt)

            # Glassy FM/additive bell + warm chorus detune (+/- 1.2 Hz stereo spread)
            mod = 0.35 * math.exp(-6.5 * dt) * math.sin(two_pi * (freq * 2.0) * dt)
            sig_l = (
                math.sin(two_pi * (freq - 0.9) * dt + mod)
                + 0.28 * math.sin(two_pi * (freq * 2.0 + 0.5) * dt) * math.exp(-3.0 * dt)
                + 0.10 * math.sin(two_pi * (freq * 3.0) * dt) * math.exp(-5.5 * dt)
            )
            sig_r = (
                math.sin(two_pi * (freq + 0.9) * dt + mod)
                + 0.28 * math.sin(two_pi * (freq * 2.0 - 0.5) * dt) * math.exp(-3.0 * dt)
                + 0.10 * math.sin(two_pi * (freq * 3.0) * dt) * math.exp(-5.5 * dt)
            )

            gain_l = amp * env * math.sqrt(0.5 * (1.0 - pan))
            gain_r = amp * env * math.sqrt(0.5 * (1.0 + pan))
            left += sig_l * gain_l
            right += sig_r * gain_r

        # Soft headroom peak around -3 dBFS, strictly click-free
        s_l = int(round(max(-1.0, min(1.0, left * master * 0.82)) * 32767.0))
        s_r = int(round(max(-1.0, min(1.0, right * master * 0.82)) * 32767.0))
        struct.pack_into("<hh", pcm, i * 4, s_l, s_r)

    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(2)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(bytes(pcm))


def main() -> None:
    repo_root = Path(__file__).resolve().parent.parent
    out_dir = repo_root / "assets" / "3ds"
    out_dir.mkdir(parents=True, exist_ok=True)

    icon_path = out_dir / "icon.png"
    banner_path = out_dir / "banner.png"
    wav_path = out_dir / "banner.wav"

    generate_icon(icon_path)
    generate_banner(banner_path)
    generate_chime_wav(wav_path)

    for p in (icon_path, banner_path, wav_path):
        print(f"generated {p.relative_to(repo_root)} ({p.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
