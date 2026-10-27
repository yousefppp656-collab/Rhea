# Rhea Brand Identity, Icon Suite & Desktop Integration Guide (`RHEA_ICON_GUIDE.md`)

This guide documents the canonical **Rhea v0.13.1** minimalist geometric **`R` monogram** icon system, its exact color and vector specification, reproducible asset generation pipeline, automated validation suite, and editor/desktop integrations across **VS Code**, **macOS**, **Windows**, and **Linux**.

## 1. Visual Design & Color Specification

The Rhea visual identity is a **flat, two-tone minimalist geometric capital `R` monogram** inside a rounded-square tile with transparent outer corners (`RGBA (0, 0, 0, 0)` at `(0, 0)`).

| Variant | Background Tile | `R` Monogram | Contrast Ratio | Primary Use Cases |
|---|---|---|---|---|
| **Primary Dark Icon** | Dark Slate-Navy **`#0F172A`** (`rgb(15, 23, 42)`) | Solid Teal **`#14B8A6`** (`rgb(20, 184, 166)`) | **7.8 : 1** (WCAG AAA) | App icon, macOS `.icns`, Windows `.ico`, Linux MIME icons, dark editor themes |
| **Light-Background Variant** | Near-White **`#F8FAFC`** (`rgb(248, 250, 252)`) | Deep Teal **`#0F766E`** (`rgb(15, 118, 110)`) | **5.4 : 1** (WCAG AA+) | Light editor themes (`tools/vscode-rhea`), light documentation surfaces |

### Canonical Vector Geometry (`512 × 512` Coordinate Space)

- **Rounded-Square Tile**: `x = 32..480`, `y = 32..480` (`width = 448`, `height = 448`, corner radius `rx = ry = 96`). Leaves a `32 px` transparent margin at `512×512` (`1 px` at `16×16`) so outer corners remain transparent (`RGBA (0, 0, 0, 0)`).
- **Geometric `R` Monogram** (`fill-rule="evenodd"`):
  - **Left Vertical Stem**: `x = 152..212` (`60 px` wide), `y = 120..392` (`272 px` tall, vertically centered at `y = 256`).
  - **Top & Middle Horizontal Bars**: `x = 212..276`, top bar `y = 120..176` (`56 px` thick), middle bar `y = 240..296` (`56 px` thick), joining the right bowl tangentially with zero seams or stair-step notches.
  - **Right Semicircular Bowl**: Centered at `(276, 208)` with outer radius `R_out = 88` (`y = 120..296`) and inner counter radius `R_in = 32` (`y = 176..240`).
  - **Diagonal Leg**: Extends cleanly from `(243, 296)..(304, 291.43)` down to `y = 392` at `x = 308..372`.
- **Strict Visual Rules Enforced by `tools/validate_icons.py`**:
  - Zero gradients (`<linearGradient>`, `<radialGradient>`), zero border strokes (`stroke=`), zero 3D bevels, zero drop shadows/glows (`<filter>`), and zero generic AI clichés.
  - Identical `<path>` geometry shared between dark and light variants.

## 2. Complete Generated Asset Inventory (`assets/icons/`)

| Asset Path | Format | Dimensions / Slots | Variant | Purpose |
|---|---|---|---|---|
| `assets/icons/rhea-logo.svg` | SVG | `512×512` (`viewBox="0 0 512 512"`) | Dark (`#0F172A` / `#14B8A6`) | Primary brand logo vector |
| `assets/icons/rhea-file-icon.svg` | SVG | `512×512` | Dark (`#0F172A` / `#14B8A6`) | Default `.rhea` source file vector icon |
| `assets/icons/rhea-file-icon-dark.svg` | SVG | `512×512` | Dark (`#0F172A` / `#14B8A6`) | Dark-theme editor file icon |
| `assets/icons/rhea-file-icon-light.svg` | SVG | `512×512` | Light (`#F8FAFC` / `#0F766E`) | Light-theme editor file icon |
| `assets/icons/rhea-icon-{16,24,32,48,64,128,256,512,1024}.png` | RGBA PNG | `16×16` .. `1024×1024` (9 files) | Dark (`#0F172A` / `#14B8A6`) | Multi-resolution dark raster suite |
| `assets/icons/rhea-icon-light-{16,24,32,48,64,128,256,512,1024}.png` | RGBA PNG | `16×16` .. `1024×1024` (9 files) | Light (`#F8FAFC` / `#0F766E`) | Multi-resolution light raster suite |
| `assets/icons/rhea-file-icon-512.png` | RGBA PNG | `512×512` | Dark (`#0F172A` / `#14B8A6`) | High-DPI file icon raster |
| `assets/icons/rhea-file-icon-1024.png` | RGBA PNG | `1024×1024` | Dark (`#0F172A` / `#14B8A6`) | Master 1024px file icon raster |
| `assets/icons/rhea.icns` | Apple ICNS | `icp4`..`ic14` (`16`..`1024` + `@2x` Retina) | Dark (`#0F172A` / `#14B8A6`) | Native macOS app & `.rhea` document bundle icon |
| `assets/icons/rhea.ico` | Windows ICO | 7 frames (`16, 24, 32, 48, 64, 128, 256` px, 32-bit PNG) | Dark (`#0F172A` / `#14B8A6`) | Native Windows Explorer `.rhea` / `.rh` icon |
| `assets/icons/rhea-social-preview.png` | RGBA PNG | `1200×640` | Dark & Light showcase | Side-by-side multi-size (`256, 128, 64, 32, 16` px) comparison sheet |

## 3. Reproducible Generation & Automated Verification

To regenerate all SVG, PNG, `.icns`, `.ico`, and VS Code extension icon assets from the canonical vector specification and verify them automatically:

```bash
python3 tools/generate_icons.py
python3 tools/validate_icons.py
```

## 4. Editor & Desktop Platform Integration

### 4.1 VS Code Extension (`tools/vscode-rhea/`)

- **Manifest**: `tools/vscode-rhea/package.json` registers `.rhea` and `.rh` extensions, `rhea.toml` and `rhea.lock` filenames, the `128×128` extension icon (`icons/rhea-icon-128.png`), and the `rhea-icons` file icon theme (`file-icon-theme.json` / `icon-theme.json`).
- **Automatic Dark/Light Switching**: Maps `.rhea`, `.rh`, `rhea.toml`, and `rhea.lock` to `icons/rhea-file-icon-dark.svg` in dark themes and `icons/rhea-file-icon-light.svg` in light themes.
- **Local Installation**:
  ```bash
  ln -s "$(pwd)/tools/vscode-rhea" ~/.vscode/extensions/vscode-rhea
  ```

### 4.2 macOS Finder & LaunchServices Integration (`tools/desktop/macos/`)

- **Files**: `tools/desktop/macos/Info.plist` (and `tools/desktop/Info.plist.rhea-uti`), `tools/desktop/macos/install-macos-icon.sh`, `tools/desktop/macos/uninstall-macos-icon.sh`.
- **Install / Uninstall**:
  ```bash
  # Dry-run validation (safe, does not modify ~/Applications):
  bash tools/desktop/macos/install-macos-icon.sh --dry-run

  # Install user-level UTI & rhea.icns bundle into ~/Applications/RheaFileAssociation.app:
  bash tools/desktop/macos/install-macos-icon.sh

  # Uninstall cleanly:
  bash tools/desktop/macos/uninstall-macos-icon.sh
  ```

### 4.3 Windows Explorer Integration (`tools/desktop/windows/`)

- **Files**: `tools/desktop/windows/register-rhea-icon.reg`, `tools/desktop/windows/unregister-rhea-icon.reg`, `tools/desktop/windows/install-windows-icon.ps1`, `tools/desktop/windows/uninstall-windows-icon.ps1`.
- **Install / Uninstall (PowerShell, Current User `HKCU:\Software\Classes` — no administrator privileges required)**:
  ```powershell
  powershell -ExecutionPolicy Bypass -File tools\desktop\windows\install-windows-icon.ps1
  powershell -ExecutionPolicy Bypass -File tools\desktop\windows\uninstall-windows-icon.ps1
  ```

### 4.4 Linux Freedesktop MIME & Hicolor Icon Integration (`tools/desktop/linux/`)

- **Files**: `tools/desktop/linux/text-x-rhea.xml` (and `tools/desktop/rhea-mime.xml`), `tools/desktop/linux/install-mime-icon.sh`, `tools/desktop/linux/uninstall-mime-icon.sh`.
- **Install / Uninstall**:
  ```bash
  # Dry-run validation:
  bash tools/desktop/linux/install-mime-icon.sh --dry-run

  # Install MIME type text/x-rhea and 16..512px + scalable SVG icons into ~/.local/share:
  bash tools/desktop/linux/install-mime-icon.sh

  # Uninstall cleanly:
  bash tools/desktop/linux/uninstall-mime-icon.sh
  ```
