#!/usr/bin/env python3
"""Automated Validator for Rhea Brand Assets, Multi-Platform Icons & Integrations.

Verifies:
  1. Canonical SVG files (valid XML, exact #0F172A/#14B8A6 dark and #F8FAFC/#0F766E
     light hex colors, identical 'R' path geometry, no gradients/filters/strokes).
  2. Multi-size RGBA PNG files (16, 24, 32, 48, 64, 128, 256, 512, 1024 dark & light,
     plus 512/1024 file icons and 1200x640 preview sheet): valid PNG headers, exact
     pixel dimensions, RGBA color_type=6, transparent outer corner (0,0), and exact
     interior background/monogram pixel colors.
  3. Native macOS `rhea.icns` container and Windows `rhea.ico` container structure
     and embedded PNG frames.
  4. VS Code extension manifest (`package.json`, `file-icon-theme.json`, `icon-theme.json`)
     and referenced icon files.
  5. macOS, Windows, and Linux desktop integration manifests and reversible install/
     uninstall scripts (`bash -n` and `--dry-run`).
"""
from __future__ import annotations

import json
import shutil
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ICONS_DIR = ROOT / "assets" / "icons"
VSCODE_DIR = ROOT / "tools" / "vscode-rhea"
DESKTOP_DIR = ROOT / "tools" / "desktop"

PNG_SIZES = (16, 24, 32, 48, 64, 128, 256, 512, 1024)
DARK_BG_RGBA = (15, 23, 42, 255)
DARK_FG_RGBA = (20, 184, 166, 255)
LIGHT_BG_RGBA = (248, 250, 252, 255)
LIGHT_FG_RGBA = (15, 118, 110, 255)


def decode_unfiltered_png(path: Path) -> tuple[int, int, int, bytes]:
    """Verify PNG signature/chunks and return (width, height, color_type, raw_decompressed_rows)."""
    assert path.exists(), f"Missing PNG file: {path}"
    data = path.read_bytes()
    assert len(data) > 32, f"Empty or truncated PNG: {path}"
    assert data[:8] == b"\x89PNG\r\n\x1a\n", f"Invalid PNG signature in {path}"
    pos = 8
    width = height = bit_depth = color_type = 0
    idat_chunks: list[bytes] = []
    seen_iend = False
    while pos < len(data):
        length = struct.unpack(">I", data[pos : pos + 4])[0]
        ctype = data[pos + 4 : pos + 8]
        chunk_data = data[pos + 8 : pos + 8 + length]
        crc_expected = struct.unpack(">I", data[pos + 8 + length : pos + 12 + length])[0]
        crc_actual = zlib.crc32(ctype + chunk_data) & 0xFFFFFFFF
        assert crc_expected == crc_actual, f"CRC mismatch in {path} chunk {ctype!r}"
        pos += 12 + length
        if ctype == b"IHDR":
            width, height, bit_depth, color_type, _, _, _ = struct.unpack(">IIBBBBB", chunk_data)
        elif ctype == b"IDAT":
            idat_chunks.append(chunk_data)
        elif ctype == b"IEND":
            seen_iend = True
            break
    assert seen_iend, f"Missing IEND chunk in {path}"
    assert bit_depth == 8, f"Expected 8-bit PNG in {path}, got {bit_depth}"
    assert color_type == 6, f"Expected RGBA (color_type=6) in {path}, got {color_type}"
    raw = zlib.decompress(b"".join(idat_chunks))
    expected_len = height * (1 + width * 4)
    assert len(raw) == expected_len, f"Decompressed length mismatch in {path}: {len(raw)} != {expected_len}"
    return width, height, color_type, raw


def sample_png_pixel(raw: bytes, width: int, x: int, y: int) -> tuple[int, int, int, int]:
    """Read RGBA pixel at (x, y) from a filter-0 raw PNG buffer."""
    row_stride = 1 + width * 4
    row_start = y * row_stride
    assert raw[row_start] == 0, f"Expected filter type 0 on row {y}, got {raw[row_start]}"
    px_offset = row_start + 1 + x * 4
    return (raw[px_offset], raw[px_offset + 1], raw[px_offset + 2], raw[px_offset + 3])


def validate_svgs() -> None:
    dark_svgs = [
        ICONS_DIR / "rhea-logo.svg",
        ICONS_DIR / "rhea-file-icon.svg",
        ICONS_DIR / "rhea-file-icon-dark.svg",
        VSCODE_DIR / "icons" / "rhea-file-icon-dark.svg",
    ]
    light_svgs = [
        ICONS_DIR / "rhea-file-icon-light.svg",
        VSCODE_DIR / "icons" / "rhea-file-icon-light.svg",
    ]

    forbidden_tokens = ("linearGradient", "radialGradient", "<filter", "feDropShadow", "feGaussianBlur", "stroke=")
    dark_paths: list[str] = []
    light_paths: list[str] = []

    for svg_path in dark_svgs:
        assert svg_path.exists(), f"Missing dark SVG: {svg_path}"
        text = svg_path.read_text(encoding="utf-8")
        for tok in forbidden_tokens:
            assert tok not in text, f"Forbidden visual token {tok!r} found in {svg_path}"
        assert "#0F172A" in text, f"Missing dark background #0F172A in {svg_path}"
        assert "#14B8A6" in text, f"Missing dark monogram #14B8A6 in {svg_path}"
        root = ET.fromstring(text)
        paths = [el.attrib.get("d", "") for el in root.iter() if el.tag.endswith("path")]
        assert len(paths) == 1 and paths[0], f"Expected 1 canonical <path> in {svg_path}"
        dark_paths.append(paths[0])

    for svg_path in light_svgs:
        assert svg_path.exists(), f"Missing light SVG: {svg_path}"
        text = svg_path.read_text(encoding="utf-8")
        for tok in forbidden_tokens:
            assert tok not in text, f"Forbidden visual token {tok!r} found in {svg_path}"
        assert "#F8FAFC" in text, f"Missing light background #F8FAFC in {svg_path}"
        assert "#0F766E" in text, f"Missing light monogram #0F766E in {svg_path}"
        root = ET.fromstring(text)
        paths = [el.attrib.get("d", "") for el in root.iter() if el.tag.endswith("path")]
        assert len(paths) == 1 and paths[0], f"Expected 1 canonical <path> in {svg_path}"
        light_paths.append(paths[0])

    assert len(set(dark_paths + light_paths)) == 1, "Dark and light SVGs must share identical R path geometry"


def validate_pngs() -> None:
    for sz in PNG_SIZES:
        for variant, prefix, expected_bg, expected_fg in (
            ("dark", "rhea-icon-", DARK_BG_RGBA, DARK_FG_RGBA),
            ("light", "rhea-icon-light-", LIGHT_BG_RGBA, LIGHT_FG_RGBA),
        ):
            p = ICONS_DIR / f"{prefix}{sz}.png"
            w, h, ctype, raw = decode_unfiltered_png(p)
            assert (w, h) == (sz, sz), f"Dimension mismatch for {p}: {(w, h)} != {(sz, sz)}"
            assert ctype == 6
            # Outer corner (0,0) must be transparent
            assert sample_png_pixel(raw, w, 0, 0) == (0, 0, 0, 0), f"Top-left corner not transparent in {p}"

            # For sizes >= 64, verify exact interior background tile and 'R' stem colors
            if sz >= 64:
                bg_x = round(100 * sz / 512)
                bg_y = round(100 * sz / 512)
                fg_x = round(180 * sz / 512)
                fg_y = round(256 * sz / 512)
                assert sample_png_pixel(raw, w, bg_x, bg_y) == expected_bg, (
                    f"{variant} background pixel mismatch in {p} at {(bg_x, bg_y)}"
                )
                assert sample_png_pixel(raw, w, fg_x, fg_y) == expected_fg, (
                    f"{variant} monogram pixel mismatch in {p} at {(fg_x, fg_y)}"
                )

    for fname, expected_dims in (
        ("rhea-file-icon-512.png", (512, 512)),
        ("rhea-file-icon-1024.png", (1024, 1024)),
        ("rhea-social-preview.png", (1200, 640)),
    ):
        w, h, ctype, raw = decode_unfiltered_png(ICONS_DIR / fname)
        assert (w, h) == expected_dims
        assert ctype == 6
        assert sample_png_pixel(raw, w, 0, 0) == (0, 0, 0, 0)


def validate_icns_and_ico() -> None:
    icns_path = ICONS_DIR / "rhea.icns"
    assert icns_path.exists(), "Missing assets/icons/rhea.icns"
    icns = icns_path.read_bytes()
    assert icns[:4] == b"icns", "Invalid ICNS magic header"
    total_len = struct.unpack(">I", icns[4:8])[0]
    assert total_len == len(icns), f"ICNS length mismatch: {total_len} != {len(icns)}"
    pos = 8
    chunk_types: list[bytes] = []
    while pos < len(icns):
        ostype = icns[pos : pos + 4]
        clen = struct.unpack(">I", icns[pos + 4 : pos + 8])[0]
        payload = icns[pos + 8 : pos + clen]
        assert payload[:8] == b"\x89PNG\r\n\x1a\n", f"ICNS chunk {ostype!r} payload is not a valid PNG"
        chunk_types.append(ostype)
        pos += clen
    for req in (b"icp4", b"icp5", b"icp6", b"ic07", b"ic08", b"ic09", b"ic10"):
        assert req in chunk_types, f"ICNS missing required slot {req!r}"

    ico_path = ICONS_DIR / "rhea.ico"
    assert ico_path.exists(), "Missing assets/icons/rhea.ico"
    ico = ico_path.read_bytes()
    reserved, ico_type, count = struct.unpack("<HHH", ico[:6])
    assert (reserved, ico_type) == (0, 1), "Invalid ICO header"
    assert count == 7, f"Expected 7 embedded sizes in rhea.ico, got {count}"
    for i in range(count):
        entry = ico[6 + i * 16 : 6 + (i + 1) * 16]
        w_b, h_b, _, _, planes, bpp, size_b, offset_b = struct.unpack("<BBBBHHII", entry)
        assert planes == 1 and bpp == 32
        payload = ico[offset_b : offset_b + size_b]
        assert payload[:8] == b"\x89PNG\r\n\x1a\n", f"ICO frame {i} is not a valid PNG"


def validate_vscode_and_desktop_integrations() -> None:
    pkg_path = VSCODE_DIR / "package.json"
    pkg = json.loads(pkg_path.read_text(encoding="utf-8"))
    assert pkg.get("icon") == "icons/rhea-icon-128.png"
    assert (VSCODE_DIR / pkg["icon"]).exists(), "Missing VS Code extension 128px icon"

    lang = pkg["contributes"]["languages"][0]
    assert ".rhea" in lang["extensions"] and ".rh" in lang["extensions"]
    assert (VSCODE_DIR / lang["icon"]["dark"]).exists()
    assert (VSCODE_DIR / lang["icon"]["light"]).exists()

    for theme_file in ("file-icon-theme.json", "icon-theme.json"):
        t_path = VSCODE_DIR / theme_file
        assert t_path.exists(), f"Missing {t_path}"
        theme = json.loads(t_path.read_text(encoding="utf-8"))
        for def_key in ("rhea_dark", "rhea_light"):
            icon_rel = theme["iconDefinitions"][def_key]["iconPath"]
            assert (VSCODE_DIR / icon_rel).exists(), f"Missing {icon_rel} referenced in {theme_file}"
        assert theme["fileExtensions"]["rhea"] == "rhea_dark"
        assert theme["light"]["fileExtensions"]["rhea"] == "rhea_light"

    # Validate macOS plists
    for plist_path in (DESKTOP_DIR / "Info.plist.rhea-uti", DESKTOP_DIR / "macos" / "Info.plist"):
        assert plist_path.exists(), f"Missing {plist_path}"
        ET.fromstring(plist_path.read_text(encoding="utf-8"))
        assert "rhea.icns" in plist_path.read_text(encoding="utf-8")
        if shutil.which("plutil"):
            r = subprocess.run(["plutil", "-lint", str(plist_path)], capture_output=True, text=True)
            assert r.returncode == 0, f"plutil -lint failed on {plist_path}: {r.stderr}"

    # Validate Windows .reg and .ps1 files
    for reg_path in (
        DESKTOP_DIR / "register-rhea-icon.reg",
        DESKTOP_DIR / "windows" / "register-rhea-icon.reg",
    ):
        txt = reg_path.read_text(encoding="utf-8")
        assert "rhea.ico,0" in txt and "Rhea.SourceFile" in txt

    for ps1_path in (
        DESKTOP_DIR / "windows" / "install-windows-icon.ps1",
        DESKTOP_DIR / "windows" / "uninstall-windows-icon.ps1",
    ):
        assert ps1_path.exists() and len(ps1_path.read_text(encoding="utf-8")) > 100

    # Validate Linux MIME XML
    for xml_path in (DESKTOP_DIR / "rhea-mime.xml", DESKTOP_DIR / "linux" / "text-x-rhea.xml"):
        assert xml_path.exists()
        ET.fromstring(xml_path.read_text(encoding="utf-8"))
        assert "*.rhea" in xml_path.read_text(encoding="utf-8")

    # Validate all shell scripts with `bash -n` and `--dry-run`
    shell_scripts = [
        DESKTOP_DIR / "install-macos-icon.sh",
        DESKTOP_DIR / "uninstall-macos-icon.sh",
        DESKTOP_DIR / "macos" / "install-macos-icon.sh",
        DESKTOP_DIR / "macos" / "uninstall-macos-icon.sh",
        DESKTOP_DIR / "linux" / "install-mime-icon.sh",
        DESKTOP_DIR / "linux" / "uninstall-mime-icon.sh",
    ]
    for sh in shell_scripts:
        assert sh.exists(), f"Missing script {sh}"
        r_syn = subprocess.run(["bash", "-n", str(sh)], capture_output=True, text=True)
        assert r_syn.returncode == 0, f"Syntax error in {sh}: {r_syn.stderr}"
        r_dry = subprocess.run(["bash", str(sh), "--dry-run"], capture_output=True, text=True)
        assert r_dry.returncode == 0, f"Dry-run failed in {sh}: {r_dry.stderr}"
        assert "[DRY-RUN]" in r_dry.stdout


def main() -> int:
    validate_svgs()
    validate_pngs()
    validate_icns_and_ico()
    validate_vscode_and_desktop_integrations()
    print("[PASS] All Rhea SVG, PNG (16..1024 dark/light), ICNS, ICO, VS Code, and Desktop integration checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
