# Rhea v0.13.1 — Public Release Notes (`RELEASE_NOTES_v0.13.1.md`)

**Release Tag**: `v0.13.1`
**Compiler & Runtime Version**: `Rhea Compiler & Runtime v0.13.1 (C ABI v2)`
**License**: `MIT` (`Copyright (c) 2026 Rhea Language Contributors`)
**Public Release Archive**: `release/Rhea_v0.13.1_Release.zip` (`334,639` bytes)
**Public Archive SHA-256**: `8d36b064ce09934fa12bc21fbcd2cd4999b9e97e88d34b520693a04537c9d326`

---

## 1. Highlights & Verified Capabilities

1. **Three Execution Backends with Cross-Backend Conformance (`32/32` Suites, `96` Executions)**:
   - **Tree-Walk Interpreter (`rhea run`)**: Full language and standard library support, including CPU `f64` tensor/autodiff kernels (`std::ai`), `try / catch`, and `pipeline` declarations.
   - **Stack Bytecode VM (`rhea vm`)**: Pre-execution bytecode verifier (`E2001`), dense integer local slot indexing, $O(1)$-memory streaming range loops, and in-place collection mutation fast paths.
   - **Native C++20 AOT Compiler (`rhea compile`)**: Emits standalone C++20 binaries with checked 64-bit signed integer arithmetic (`E1002`), division/modulo-by-zero traps (`E1001`), algebraic `enum` and `match` lowering, generic functions/structs, and lexical closures.
2. **Comprehensive Compiler & Security Hardening (`D-01`..`D-20`, `LSP-01`)**:
   - UTF-8 codepoint string splitting on empty delimiters (`std::string.split(s, "")`), bounded recursion guards in both the Rhea parser (`MAX_PARSE_DEPTH = 256`, `E0002`) and JSON parser (`E1006`), `INT64_MIN` literal and `INT64_MIN % -1` UB guards (`E1002`), C++20 keyword and 3-digit octal string escaping in `NativeCodegen`, `16 MB` `Content-Length` cap in `rhea lsp`, and direct `fork()`/`execvp()` compiler invocation without `/bin/sh`.
3. **Minimalist Geometric `R` Monogram Brand & Multi-Platform Desktop Suite**:
   - **Primary Dark Variant**: Dark slate-navy `#0F172A` tile + solid teal `#14B8A6` `R` monogram.
   - **Light-Background Variant**: Near-white `#F8FAFC` tile + deep teal `#0F766E` `R` monogram.
   - Includes editable SVGs, `16×16`..`1024×1024` RGBA PNGs (dark & light), native macOS `rhea.icns`, native Windows `rhea.ico`, VS Code extension (`tools/vscode-rhea/`), and reversible macOS, Windows, and Linux desktop integration scripts (`tools/desktop/`).

---

## 2. Measured Performance Improvements (`benchmarks/run_benchmarks.py`)

Measured across 2 warm-up iterations and 5 timed iterations using monotonic timers (`time.perf_counter()`):

| Workload | `v0.12.0` Baseline | `v0.13.x` Optimized | Measured Speedup |
|---|---|---|---|
| `rhea vm` — `100,000`-Iteration Range Loop (`1..=100000`) | `496.71 ms` | **`1.93 ms`** | **`257.38x`** |
| `rhea vm` — `10,000`-Element List `push` + Index Sum | `356.65 ms` | **`1.75 ms`** | **`203.80x`** |
| `rhea run` — `10,000`-Element List `push` + Index Sum | `256.82 ms` | **`6.97 ms`** | **`36.85x`** |
| `rhea run` — Recursive `fib(22)` (`57,313` calls) | `149.74 ms` | **`10.10 ms`** | **`14.83x`** |

---

## 3. Supported Build Requirements & Verification Summary

- **Required Dependencies**: POSIX environment (`macOS` or `Linux`), `make`, a C++20 compiler (`clang++` 14+ or `g++` 11+ accessible as `c++`), and `python3` (3.9+).
- **Verification Results**:
  - `make clean && make all && make test-asan` $\rightarrow$ **PASS (`0` warnings, `0` ASan/UBSan errors)**
  - `./bin/rhea test` $\rightarrow$ **`165 / 165` automated tests passed (`0` failures)**
  - `python3 tests/differential_harness.py` $\rightarrow$ **`32 / 32` cross-backend suites (`96` executions) passed (`0` failures)**
  - `python3 tools/validate_icons.py` $\rightarrow$ **PASS across all SVG, PNG, ICNS, ICO, VS Code, and desktop integration checks**

---

## 4. Known Limitations & Explicitly Untested Components

- **Backend Scope Differences**: `std::ai.*` tensor kernels, `try / catch`, and `pipeline` declarations are supported in `rhea run` (and `std::json.*` in `rhea run` and `rhea vm`) and rejected upfront with `E2002` in `rhea compile`.
- **Untested / Environment-Dependent Components**:
  - The 13-crate experimental Rust workspace (`crates/rhea_*`) requires an external `cargo`/`rustc` installation and was not compiled on the build host.
  - VS Code extension manifests (`tools/vscode-rhea/`) were validated statically via `tools/validate_icons.py` without `vsce` packaging.
  - Windows registry (`.reg`) and PowerShell (`.ps1`) scripts in `tools/desktop/windows/` were validated structurally rather than executed on a live Windows host.
