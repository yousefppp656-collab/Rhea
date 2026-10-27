# Rhea v0.13.1 — Clean-Checkout & Publication Verification Report (`PUBLICATION_VERIFICATION.md`)

This document records the exact commands, exit codes, test counts, privacy scans, and limitations verified on the standalone GitHub publication copy (`Rhea_GitHub_Publication`).

---

## 1. Executed Build, Sanitizer, Conformance & Icon Verification

All commands below were executed directly in the clean publication directory and in an isolated temporary copy outside the working tree:

| Step | Command Executed | Exit Code | Verified Result | Status |
|---|---|---|---|---|
| **1. Clean Build** | `make clean && make all` | `0` | Built `lib/librhea_native.so`, `bin/rhea`, and `bin/rhea_cpp_tests` with `-std=c++20 -O3 -Wall -Wextra -Wpedantic -fPIC -ffile-prefix-map=$(CURDIR)=.` (`0` warnings, `0` errors) | **PASSED** |
| **2. Version & ABI Check** | `./bin/rhea version` | `0` | Output: `Rhea Compiler & Runtime v0.13.1 (C ABI v2)` | **PASSED** |
| **3. Lockfile Integrity** | `./bin/rhea pkg verify` | `0` | Output: `[rhea pkg] Verified rhea.toml & rhea.lock` | **PASSED** |
| **4. C++20 Native Kernel Suite** | `./bin/rhea_cpp_tests` | `0` | Output: `[PASS] All C++20 native kernel, autodiff, Adam, and checkpoint integrity tests passed.` | **PASSED** |
| **5. ASan + UBSan Suite** | `make test-asan` | `0` | Built and executed `bin/rhea_cpp_tests_asan` and `bin/rhea_asan` under `-fsanitize=address,undefined` (`0` memory leaks, `0` UB findings) | **PASSED** |
| **6. Icon & Integration Validator** | `python3 tools/validate_icons.py` | `0` | Verified all dark/light SVGs, `18` multi-size RGBA PNGs (`16..1024` px), `rhea.icns`, `rhea.ico`, VS Code manifests, `plutil -lint`, and `bash -n` / `--dry-run` desktop scripts | **PASSED** |
| **7. Cross-Backend Differential Harness** | `python3 tests/differential_harness.py` | `0` | **`32 / 32` suites passed (`96` backend executions across `run`, `vm`, and `compile`, `0` failures)** | **PASSED** |
| **8. Full End-to-End Test Suite** | `./bin/rhea test` | `0` | **`165 / 165` automated tests passed in `128.53s` (`0` failures, `0` skipped)** | **PASSED** |
| **9. Release Archive Integrity** | `unzip -t release/Rhea_v0.13.1_Release.zip` | `0` | `No errors detected in compressed data of release/Rhea_v0.13.1_Release.zip.` (`334,639` bytes, SHA-256 `8d36b064ce09934fa12bc21fbcd2cd4999b9e97e88d34b520693a04537c9d326`) | **PASSED** |
| **10. Privacy & Zero-Leak Scan** | Recursive search for personal usernames, local home-directory paths, private workspace directory names, and secrets across all tracked files and `release/Rhea_v0.13.1_Release.zip` | `0` | **`0` matches across all files in `Rhea_GitHub_Publication` (including compiled ASan binaries via `-ffile-prefix-map`)** | **PASSED** |

---

## 2. Classification of Checks

### 2.1 Passed Checks (Executed on Host)
- C++20 compiler, lexer, parser, module resolver, static type checker, interpreter (`rhea run`), bytecode verifier & VM (`rhea vm`), native C++20 code generator (`rhea compile`), formatter (`rhea fmt`), linter (`rhea lint`), debugger (`rhea debug`), JSON-RPC Language Server (`rhea lsp`), and package lockfile verifier (`rhea pkg`).
- C++20 native tensor, reverse-mode autodiff, Adam optimizer, and FNV-1a checkpoint integrity library (`librhea_native.so`) and Python `ctypes` bridge (`python/rhea_ai`).
- All 15 main `.rhea` example programs (`examples/01`–`15`) and all 10 readability benchmark programs (`examples/readability/01`–`10`).
- Reproducible icon generation (`tools/generate_icons.py`) and validation (`tools/validate_icons.py`), macOS `plutil -lint`, and macOS/Linux script `--dry-run` checks.

### 2.2 Checks Inspected Statically (Not Executed Due to Host OS / Missing External Tooling)
- **Windows Registry & PowerShell Scripts (`tools/desktop/windows/`)**: Inspected and validated for syntax and `rhea.ico,0` path references via `tools/validate_icons.py`; not executed live because the verification host runs macOS.
- **Linux `update-mime-database` / `gtk-update-icon-cache` (`tools/desktop/linux/`)**: Shell scripts passed `bash -n` and `--dry-run`; live Freedesktop database updates run only on Linux desktop hosts.
- **VS Code Extension Packaging (`tools/vscode-rhea/`)**: `package.json`, `file-icon-theme.json`, `icon-theme.json`, TextMate grammar, and SVG/PNG icons were validated via `tools/validate_icons.py`; `vsce package` could not run because `vsce` is not installed.
- **Experimental Rust Workspace (`crates/rhea_*`)**: Source files are included for reference, but `cargo` / `rustc` are not installed on the verification host.
