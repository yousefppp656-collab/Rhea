# Rhea v0.11.0 — Final Release Completion & Feature Matrix

## 1. Release Summary
- **Version**: `v0.11.0` (C ABI `v2`)
- **Repository Root**: `.` (project root)
- **Build Verification**: `make clean && make all && make test-asan` -> **PASS** (`0` warnings, `0` errors under `-std=c++20 -O3 -Wall -Wextra -Wpedantic`)
- **Automated Regression Suite (`./bin/rhea test`)**: **120 / 120 PASS** (`17.77s`)
- **Cross-Backend Differential Suite (`python3 tests/differential_harness.py`)**: **24 / 24 PASS** across `rhea run` (Tree-Walking Interpreter), `rhea vm` (Stack-Based Bytecode VM), and `rhea compile` (Native C++20 Compiler)
- **ASan + UBSan C++20 Kernel & Autodiff Suite (`make test-asan`)**: **5 / 5 PASS** (`0` memory leaks, `0` undefined behavior)

## 2. Feature Classification Matrix

| Subsystem / Feature | Status | Verification Evidence |
|---|---|---|
| Concise bindings (`name = expr`, `mut count = 0`) & Damerau-Levenshtein typo guard (`E0004`) | **Stable and tested** | Tests `41`, `42`, `56`, `57` |
| Lexical block scoping (`{ ... }`) & shadowing across `run`, `vm`, `compile` | **Stable and tested** | Test `58`, Differential Suite `01` |
| Return-path completeness (`E0007`), optional `main` return, implicit final-expression return | **Stable and tested** | Tests `54`, `61`, `62`, Differential Suite `02` |
| Recursive functions & `CallFrame` stack across `run`, `vm`, `compile` | **Stable and tested** | Test `63`, Differential Suite `02` |
| First-class lexical closures (`|x: Int| x + factor`) with capture-by-value snapshots | **Stable and tested** | Tests `12`, `64`, `65`, Differential Suite `03` |
| Generic functions (`fn id<T>(x: T) -> T`), generic structs (`struct Pair<T>`), body operator checking (`E0003`) | **Stable and tested** | Tests `66`, `67`, `81`, Differential Suite `04` |
| Cross-backend `Map<K, V>` creation, indexing, mutation (`m[k] = v`), missing-key `E1003`, `has_key`, `keys`, `values`, `remove_key`, empty typed maps | **Stable and tested** | Tests `76`–`80`, Differential Suites `09`–`11` |
| Local multi-file modules (`module`, `import`, `pub fn`, `priv fn`, circular import & `..` traversal guards `E0009`) | **Stable and tested** | Tests `68`–`70`, Differential Suite `14` |
| Overflow-safe inclusive (`..=`) & exclusive (`..`) ranges (`INT64_MAX..=INT64_MAX`) | **Stable and tested** | Tests `36`–`40`, `71`, `72`, Differential Suite `05` |
| Interactive terminal I/O (`input`, `is_eof`, `try_parse_int`) & monotonic timing (`sleep(1s)`, `now_ms`) | **Stable and tested** | Tests `44`–`53`, Differential Suite `07` |
| Real deterministic AST/token formatter (`rhea fmt`, `rhea fmt --check`, `rhea fmt -w`) | **Stable and tested** | Tests `33`, `82`–`85` |
| Stateful JSON-RPC Language Server (`rhea lsp`: live diagnostics, hover, completion, definition, documentSymbol) | **Stable and tested** | Tests `31`, `86` |
| Project scaffolding & CLI workflow (`rhea new`, `rhea check`, `rhea run`, `rhea build`, `rhea test`, `rhea pkg`) | **Stable and tested** | Tests `26`, `87` |
| Practical Standard Library (`std::json`, `std::string` UTF-8 `char_count`, `std::path`, `std::env`, `std::fs`, `std::log`, `std::collections`, `std::ai`) | **Stable and tested** | Tests `88`–`93`, Examples `01`–`15` |
| Minimalist `.rhea` icon suite (SVG + 16..1024px RGBA PNG + native macOS `rhea.icns` + VS Code integration) | **Stable and tested** | Tests `35`, `94`, `/usr/bin/sips` verification |
| Multi-dimensional generic specialization (>2 type parameters) & higher-order closure capture mutation | **Implemented but experimental** | Documented in `docs/LANGUAGE_SPECIFICATION.md` |
| Rust workspace (`cargo build`) & VS Code `.vsix` binary packaging (`vsce package`) | **Blocked by environment** | `rustc`/`cargo` and `vsce` are not installed on this host |
