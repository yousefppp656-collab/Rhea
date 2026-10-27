#!/usr/bin/env python3
"""Reproducible Icon & Brand Asset Generator for the Rhea Programming Language.

Generates the complete Rhea minimalist geometric 'R' monogram icon suite from a
single canonical vector specification:
  - Primary Dark Variant:
      * Background tile: Dark slate-navy #0F172A (RGB 15, 23, 42)
      * Monogram 'R':    Solid teal      #14B8A6 (RGB 20, 184, 166)
  - Light-Background Variant:
      * Background tile: Near-white      #F8FAFC (RGB 248, 250, 252)
      * Monogram 'R':    Deep teal       #0F766E (RGB 15, 118, 110)

Design Rules Enforced:
  - Flat two-tone design (no gradients, no border strokes, no 3D bevels, no shadows).
  - Exact same core 'R' geometry and proportions across dark and light variants.
  - Transparent outer corners (RGBA (0, 0, 0, 0) at (0, 0)) for clean desktop and
    editor rendering across macOS, Windows, Linux, and VS Code.
  - Multi-size RGBA PNG exports: 16, 24, 32, 48, 64, 128, 256, 512, 1024 px.
  - Native macOS .icns bundle (assets/icons/rhea.icns) and Windows .ico file
    (assets/icons/rhea.ico).
  - Multi-size dark/light comparison preview sheet (assets/icons/rhea-social-preview.png).
"""
from __future__ import annotations

import math
import shutil
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ICONS_DIR = ROOT / "assets" / "icons"
VSCODE_ICONS_DIR = ROOT / "tools" / "vscode-rhea" / "icons"

# Exact brand color specification
DARK_BG_HEX = "#0F172A"
DARK_FG_HEX = "#14B8A6"
DARK_BG_RGB = (15, 23, 42)
DARK_FG_RGB = (20, 184, 166)

LIGHT_BG_HEX = "#F8FAFC"
LIGHT_FG_HEX = "#0F766E"
LIGHT_BG_RGB = (248, 250, 252)
LIGHT_FG_RGB = (15, 118, 110)

PNG_SIZES = (16, 24, 32, 48, 64, 128, 256, 512, 1024)


def make_svg(bg_hex: str, fg_hex: str, title: str) -> str:
    """Return canonical, hand-editable flat two-tone SVG markup in 512x512 space."""
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" width="512" height="512" role="img" aria-label="{title}">
  <title>{title}</title>
  <rect x="32" y="32" width="448" height="448" rx="96" ry="96" fill="{bg_hex}"/>
  <path fill="{fg_hex}" fill-rule="evenodd" d="M 152 120 H 276 A 88 88 0 0 1 304 291.43 L 372 392 H 308 L 243 296 H 212 V 392 H 152 Z M 212 176 V 240 H 276 A 32 32 0 0 0 276 176 Z"/>
</svg>
"""


def sd_rounded_box(px: float, py: float, cx: float, cy: float, half_w: float, half_h: float, r: float) -> float:
    """Signed distance to a rounded rectangle centered at (cx, cy). Negative inside."""
    qx = abs(px - cx) - (half_w - r)
    qy = abs(py - cy) - (half_h - r)
    out_dist = math.hypot(max(qx, 0.0), max(qy, 0.0))
    in_dist = min(max(qx, qy), 0.0)
    return out_dist + in_dist - r


def point_in_r_monogram(x: float, y: float) -> bool:
    """Exact analytical hit test for the canonical 'R' monogram in 512x512 coordinates.

    Matches the SVG path:
      M 152 120 H 276 A 88 88 0 0 1 304 291.43 L 372 392 H 308 L 243 296 H 212 V 392 H 152 Z
      M 212 176 V 240 H 276 A 32 32 0 0 0 276 176 Z
    with zero seams or notches between bars, bowl, and diagonal leg.
    """
    if x < 152.0 or x > 372.0 or y < 120.0 or y > 392.0:
        return False

    # 1. Left vertical stem
    if 152.0 <= x <= 212.0 and 120.0 <= y <= 392.0:
        return True

    # 2. Top and middle horizontal bars connecting stem to bowl center x=276
    if 212.0 <= x <= 276.0:
        if 120.0 <= y <= 176.0 or 240.0 <= y <= 296.0:
            return True

    # 3. Right semicircular bowl around (276, 208) with outer R=88, inner R=32
    if x >= 276.0 and 120.0 <= y <= 296.0:
        dx = x - 276.0
        dy = y - 208.0
        d2 = dx * dx + dy * dy
        if 32.0 * 32.0 <= d2 <= 88.0 * 88.0:
            return True

    # 4. Diagonal leg from middle bar / bowl junction down to y=392
    # Outer edge passes through (304, 291.4266) -> (372, 392)
    # Inner edge passes through (243, 296) -> (308, 392)
    # Extended slightly upward to y=268 inside the solid bowl/bar junction so there is no seam.
    if 268.0 <= y <= 392.0:
        x_left = 243.0 + (308.0 - 243.0) * (y - 296.0) / (392.0 - 296.0)
        x_right = 304.0 + (372.0 - 304.0) * (y - 291.4266) / (392.0 - 291.4266)
        if x_left <= x <= x_right:
            # Exclude the inner counter hole above y=240
            return True

    return False


def encode_rgba_png(width: int, height: int, rgba_bytes: bytes) -> bytes:
    """Encode raw RGBA8888 buffer into a standard PNG (color_type=6)."""
    stride = width * 4
    raw_rows = bytearray()
    for y in range(height):
        raw_rows.append(0)  # Filter type 0 (None)
        row_start = y * stride
        raw_rows.extend(rgba_bytes[row_start : row_start + stride])

    def make_chunk(chunk_type: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(chunk_type + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + chunk_type + data + struct.pack(">I", crc)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    idat = zlib.compress(bytes(raw_rows), level=9)
    return b"\x89PNG\r\n\x1a\n" + make_chunk(b"IHDR", ihdr) + make_chunk(b"IDAT", idat) + make_chunk(b"IEND", b"")


def render_icon_rgba(size: int, bg_rgb: tuple[int, int, int], fg_rgb: tuple[int, int, int]) -> bytes:
    """Render the canonical Rhea icon at `size x size` using 4x4 supersampling."""
    scale = 512.0 / float(size)
    # Use 4x4 subpixel grid for crisp anti-aliased edges at all sizes (especially 16..64px)
    grid = 4
    inv_samples = 1.0 / (grid * grid)
    sub_offsets = [(i + 0.5) / grid for i in range(grid)]

    buf = bytearray(size * size * 4)
    bg_r, bg_g, bg_b = bg_rgb
    fg_r, fg_g, fg_b = fg_rgb

    idx = 0
    for py in range(size):
        for px in range(size):
            # Fast bounding checks at pixel center in 512-space
            cx = (px + 0.5) * scale
            cy = (py + 0.5) * scale
            dist_tile = sd_rounded_box(cx, cy, 256.0, 256.0, 224.0, 224.0, 96.0)

            if dist_tile > scale * 1.1:
                # Completely outside tile -> transparent (0, 0, 0, 0)
                idx += 4
                continue

            # Check if pixel is strictly inside the tile and far from R boundaries for fast path
            tile_hits = 0
            fg_hits = 0
            for oy in sub_offsets:
                sy = (py + oy) * scale
                for ox in sub_offsets:
                    sx = (px + ox) * scale
                    if sd_rounded_box(sx, sy, 256.0, 256.0, 224.0, 224.0, 96.0) <= 0.0:
                        tile_hits += 1
                        if point_in_r_monogram(sx, sy):
                            fg_hits += 1

            if tile_hits == 0:
                idx += 4
                continue

            alpha_f = tile_hits * inv_samples
            fg_ratio = fg_hits / float(tile_hits)
            r = round(bg_r * (1.0 - fg_ratio) + fg_r * fg_ratio)
            g = round(bg_g * (1.0 - fg_ratio) + fg_g * fg_ratio)
            b = round(bg_b * (1.0 - fg_ratio) + fg_b * fg_ratio)
            a = round(255.0 * alpha_f)

            buf[idx] = r
            buf[idx + 1] = g
            buf[idx + 2] = b
            buf[idx + 3] = a
            idx += 4

    return bytes(buf)


def blit_rgba(
    canvas: bytearray,
    canvas_w: int,
    canvas_h: int,
    src: bytes,
    src_w: int,
    src_h: int,
    dst_x: int,
    dst_y: int,
) -> None:
    """Alpha-composite `src` RGBA image onto `canvas` at (dst_x, dst_y)."""
    for y in range(src_h):
        cy = dst_y + y
        if cy < 0 or cy >= canvas_h:
            continue
        for x in range(src_w):
            cx = dst_x + x
            if cx < 0 or cx >= canvas_w:
                continue
            s_idx = (y * src_w + x) * 4
            sr, sg, sb, sa = src[s_idx], src[s_idx + 1], src[s_idx + 2], src[s_idx + 3]
            if sa == 0:
                continue
            d_idx = (cy * canvas_w + cx) * 4
            dr, dg, db, da = canvas[d_idx], canvas[d_idx + 1], canvas[d_idx + 2], canvas[d_idx + 3]
            sa_f = sa / 255.0
            da_f = da / 255.0
            out_a = sa_f + da_f * (1.0 - sa_f)
            if out_a <= 0.0:
                continue
            out_r = round((sr * sa_f + dr * da_f * (1.0 - sa_f)) / out_a)
            out_g = round((sg * sa_f + dg * da_f * (1.0 - sa_f)) / out_a)
            out_b = round((sb * sa_f + db * da_f * (1.0 - sa_f)) / out_a)
            canvas[d_idx] = out_r
            canvas[d_idx + 1] = out_g
            canvas[d_idx + 2] = out_b
            canvas[d_idx + 3] = round(out_a * 255.0)


def render_preview_sheet(
    dark_buffers: dict[int, bytes],
    light_buffers: dict[int, bytes],
) -> bytes:
    """Render a 1200x640 RGBA multi-size preview sheet showing Dark & Light variants.

    Top-left corner (0,0) is kept transparent (0,0,0,0) to preserve RGBA outer-corner
    transparency checks, while displaying two clean comparison panels:
      - Left panel (dark surface showcase): 256, 128, 64, 32, 16 px primary dark icons
      - Right panel (light surface showcase): 256, 128, 64, 32, 16 px light-variant icons
    """
    w, h = 1200, 640
    canvas = bytearray(w * h * 4)

    # Outer rounded card (x: 16..1184, y: 16..624, rx: 36) -> leaves (0,0) transparent
    for y in range(h):
        for x in range(w):
            if sd_rounded_box(x + 0.5, y + 0.5, 600.0, 320.0, 584.0, 304.0, 36.0) <= 0.0:
                idx = (y * w + x) * 4
                if x < 600:
                    # Subtle slate backdrop (#1E293B) so the #0F172A dark tile silhouette stands out clearly
                    canvas[idx] = 30
                    canvas[idx + 1] = 41
                    canvas[idx + 2] = 59
                    canvas[idx + 3] = 255
                else:
                    # Subtle cool-gray backdrop (#E2E8F0) so the #F8FAFC light tile silhouette stands out clearly
                    canvas[idx] = 226
                    canvas[idx + 1] = 232
                    canvas[idx + 2] = 240
                    canvas[idx + 3] = 255

    # Blit Dark variant hierarchy on the left panel (x=0..600)
    blit_rgba(canvas, w, h, dark_buffers[256], 256, 256, 68, 120)
    blit_rgba(canvas, w, h, dark_buffers[128], 128, 128, 364, 120)
    blit_rgba(canvas, w, h, dark_buffers[64], 64, 64, 364, 280)
    blit_rgba(canvas, w, h, dark_buffers[32], 32, 32, 448, 312)
    blit_rgba(canvas, w, h, dark_buffers[16], 16, 16, 500, 328)

    # Bottom accent bar on left panel in #14B8A6
    for y in range(440, 452):
        for x in range(68, 516):
            idx = (y * w + x) * 4
            canvas[idx], canvas[idx + 1], canvas[idx + 2], canvas[idx + 3] = 20, 184, 166, 255

    # Blit Light variant hierarchy on the right panel (x=600..1200)
    blit_rgba(canvas, w, h, light_buffers[256], 256, 256, 668, 120)
    blit_rgba(canvas, w, h, light_buffers[128], 128, 128, 964, 120)
    blit_rgba(canvas, w, h, light_buffers[64], 64, 64, 964, 280)
    blit_rgba(canvas, w, h, light_buffers[32], 32, 32, 1048, 312)
    blit_rgba(canvas, w, h, light_buffers[16], 16, 16, 1100, 328)

    # Bottom accent bar on right panel in #0F766E
    for y in range(440, 452):
        for x in range(668, 1116):
            idx = (y * w + x) * 4
            canvas[idx], canvas[idx + 1], canvas[idx + 2], canvas[idx + 3] = 15, 118, 110, 255

    return encode_rgba_png(w, h, bytes(canvas))


def build_icns(png_by_size: dict[int, bytes]) -> bytes:
    """Build a standard Apple ICNS container with PNG-compressed icon slots."""
    # Standard modern macOS ICNS chunk types supporting embedded PNG payloads
    slots = [
        (b"icp4", 16),    # 16x16
        (b"icp5", 32),    # 32x32
        (b"icp6", 64),    # 64x64
        (b"ic07", 128),   # 128x128
        (b"ic08", 256),   # 256x256
        (b"ic09", 512),   # 512x512
        (b"ic10", 1024),  # 1024x1024 (512x512@2x)
        (b"ic11", 32),    # 16x16@2x
        (b"ic12", 64),    # 32x32@2x
        (b"ic13", 256),   # 128x128@2x
        (b"ic14", 512),   # 256x256@2x
    ]
    chunks = bytearray()
    for ostype, sz in slots:
        payload = png_by_size[sz]
        chunks.extend(ostype)
        chunks.extend(struct.pack(">I", len(payload) + 8))
        chunks.extend(payload)

    total_len = len(chunks) + 8
    return b"icns" + struct.pack(">I", total_len) + bytes(chunks)


def build_ico(png_by_size: dict[int, bytes]) -> bytes:
    """Build a standard Windows .ico container embedding 16, 24, 32, 48, 64, 128, and 256 px PNG frames."""
    ico_sizes = [16, 24, 32, 48, 64, 128, 256]
    header = struct.pack("<HHH", 0, 1, len(ico_sizes))
    dir_entries = bytearray()
    payloads = bytearray()
    offset = 6 + 16 * len(ico_sizes)

    for sz in ico_sizes:
        png_data = png_by_size[sz]
        w_byte = 0 if sz >= 256 else sz
        h_byte = 0 if sz >= 256 else sz
        entry = struct.pack(
            "<BBBBHHII",
            w_byte,
            h_byte,
            0,   # palette count
            0,   # reserved
            1,   # color planes
            32,  # bits per pixel (RGBA)
            len(png_data),
            offset,
        )
        dir_entries.extend(entry)
        payloads.extend(png_data)
        offset += len(png_data)

    return header + bytes(dir_entries) + bytes(payloads)


def main() -> None:
    ICONS_DIR.mkdir(parents=True, exist_ok=True)
    VSCODE_ICONS_DIR.mkdir(parents=True, exist_ok=True)

    # 1. Write canonical editable SVG files (dark primary & light variant)
    dark_svg = make_svg(DARK_BG_HEX, DARK_FG_HEX, "Rhea Programming Language Icon (Dark)")
    light_svg = make_svg(LIGHT_BG_HEX, LIGHT_FG_HEX, "Rhea Programming Language Icon (Light)")

    for name in ("rhea-logo.svg", "rhea-file-icon.svg", "rhea-file-icon-dark.svg"):
        (ICONS_DIR / name).write_text(dark_svg, encoding="utf-8")
    (ICONS_DIR / "rhea-file-icon-light.svg").write_text(light_svg, encoding="utf-8")

    # Sync SVGs into VS Code extension icons directory
    (VSCODE_ICONS_DIR / "rhea-file-icon-dark.svg").write_text(dark_svg, encoding="utf-8")
    (VSCODE_ICONS_DIR / "rhea-file-icon-light.svg").write_text(light_svg, encoding="utf-8")

    # 2. Render multi-size RGBA PNGs for both Dark and Light variants
    dark_raw_buffers: dict[int, bytes] = {}
    light_raw_buffers: dict[int, bytes] = {}
    dark_png_bytes: dict[int, bytes] = {}
    light_png_bytes: dict[int, bytes] = {}

    for sz in PNG_SIZES:
        d_raw = render_icon_rgba(sz, DARK_BG_RGB, DARK_FG_RGB)
        l_raw = render_icon_rgba(sz, LIGHT_BG_RGB, LIGHT_FG_RGB)
        d_png = encode_rgba_png(sz, sz, d_raw)
        l_png = encode_rgba_png(sz, sz, l_raw)

        dark_raw_buffers[sz] = d_raw
        light_raw_buffers[sz] = l_raw
        dark_png_bytes[sz] = d_png
        light_png_bytes[sz] = l_png

        (ICONS_DIR / f"rhea-icon-{sz}.png").write_bytes(d_png)
        (ICONS_DIR / f"rhea-icon-light-{sz}.png").write_bytes(l_png)

    # Maintain backward-compatible canonical 512 and 1024 file icon paths
    (ICONS_DIR / "rhea-file-icon-512.png").write_bytes(dark_png_bytes[512])
    (ICONS_DIR / "rhea-file-icon-1024.png").write_bytes(dark_png_bytes[1024])

    # Copy 128x128 marketplace/extension icon into tools/vscode-rhea/icons/
    shutil.copyfile(ICONS_DIR / "rhea-icon-128.png", VSCODE_ICONS_DIR / "rhea-icon-128.png")

    # 3. Generate native macOS .icns and Windows .ico packages
    (ICONS_DIR / "rhea.icns").write_bytes(build_icns(dark_png_bytes))
    (ICONS_DIR / "rhea.ico").write_bytes(build_ico(dark_png_bytes))

    # 4. Generate 1200x640 multi-size dark/light preview card
    preview_png = render_preview_sheet(dark_raw_buffers, light_raw_buffers)
    (ICONS_DIR / "rhea-social-preview.png").write_bytes(preview_png)

    print(f"[OK] Generated Rhea icon suite in {ICONS_DIR}")
    print(f"     - SVGs: rhea-logo.svg, rhea-file-icon.svg, rhea-file-icon-dark.svg, rhea-file-icon-light.svg")
    print(f"     - Dark PNGs:  {', '.join(f'{s}x{s}' for s in PNG_SIZES)}")
    print(f"     - Light PNGs: {', '.join(f'{s}x{s}' for s in PNG_SIZES)}")
    print(f"     - Bundles:    rhea.icns ({len((ICONS_DIR / 'rhea.icns').read_bytes())} B), rhea.ico ({len((ICONS_DIR / 'rhea.ico').read_bytes())} B)")
    print(f"     - Preview:    rhea-social-preview.png ({len(preview_png)} B)")


if __name__ == "__main__":
    main()
