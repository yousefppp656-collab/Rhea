# Rhea v0.12.0 — Developer Experience, Diagnostics, Ergonomics & Tooling Audit (`RHEA_DX_AUDIT.md`)

## 1. Initial Inspection of the `v0.11.0` Repository

Before making any code changes for Stage 2 (`v0.12.0`), the existing repository was inspected and the baseline test suites were executed:
- **Compiler & Runtime Version**: `Rhea Compiler & Runtime v0.11.0 (C ABI v2)` (`native/cpp/src/rhea_driver.cpp`, `7,315` lines).
- **Baseline Differential Suite (`python3 tests/differential_harness.py`)**: `24 / 24` cross-backend suites passed (`0` failures).
- **Baseline Regression Suite (`./bin/rhea test`)**: `120 / 120` automated tests passed (`0` failures).
- **Existing Audit & Conformance Docs**: `RHEA_CORRECTNESS_AUDIT.md`, `RHEA_LANGUAGE_CONFORMANCE.md`, `VERIFICATION_REPORT.md`, `docs/LANGUAGE_SPECIFICATION.md`, and `docs/STANDARD_LIBRARY.md` are present and consistent with `v0.11.0`.

## 2. Developer Experience, Diagnostics & Tooling Gaps Identified

Our audit of `native/cpp/src/rhea_driver.cpp` and the CLI/LSP/formatter workflows identified seven high-leverage areas where developer experience, ergonomics, and tooling consistency need improvement:

### Gap 1: Chained / Nested Collection & Struct Lvalue Mutation (`matrix[i][j] = v`, `obj.items[i] = v`)
- **Current State in `v0.11.0`**: `TypeChecker::check_stmt`, `Interpreter::exec_stmt`, and `BytecodeCompiler::compile_stmt` only inspect single-level `Index` (`xs[i] = v`) or `FieldAccess` (`s.f = v`) where `target->lhs` is a direct `Ident`. Writing `matrix[i][j] = 99`, `matrix[i][j] += 5`, `obj.items[i] = 42`, or `obj.inner.field = 7` bypasses root mutability checking in `TypeChecker` and is silently ignored by `Interpreter` and `BytecodeVM`.
- **v0.12.0 Engineering Solution**:
  - Implement general lvalue chain walking (`extract_lvalue_root_ident`) in `TypeChecker::check_stmt` to verify that the root binding exists (`E0004`), is declared `mut` (`E0005`), and has its shadow name resolved (`resolved_name`), while validating every intermediate index/field type and leaf RHS compatibility (`E0003`).
  - Implement in-place pointer path traversal in `Interpreter::exec_stmt` and `OpCode::LValueSet` in `BytecodeCompiler`/`BytecodeVM`, evaluating each index expression **exactly once** in left-to-right order and enforcing `E1003` bounds/key checks at every step.
  - Implement `rhea_index_ref` and single-evaluation `{ auto& __rhea_lv = ...; }` lowering in `NativeCodegen` so `rhea run`, `rhea vm`, and `rhea compile` behave identically on nested mutations and compound assignments.

### Gap 2: Implicit Closure Mutation Trap on Captured Outer Variables
- **Current State in `v0.11.0`**: Rhea closures capture outer variables by lexical value snapshot (`[=]` in C++20, value map copy in `Interpreter` and `BytecodeVM`). However, if a closure body attempts to reassign or mutate an outer `mut` variable (`counter += 1` inside `|x: Int| { counter += x; return counter }`), `TypeChecker` currently permits it because `counter` is marked `is_mutable` in the outer scope. At runtime, `rhea run` and `rhea vm` mutate a temporary copy that is discarded on return, while `rhea compile` fails inside `clang++` (`cannot assign to a variable captured by copy in a non-mutable lambda`).
- **v0.12.0 Engineering Solution**:
  - Track `closure_scope_floor_` in `TypeChecker` so any attempt inside a closure to reassign or mutate (`=`, `+=`, `xs[i] = ...`, `push(xs, ...)`) a variable declared outside the closure is rejected at compile time with a clear, actionable `E0005` diagnostic explaining Rhea's lexical value-snapshot capture model and showing how to return updated state explicitly.

### Gap 3: Silent or Opaque Failures on Backend-Specific Features (`rhea vm` & `rhea compile`)
- **Current State in `v0.11.0`**:
  - `std::path.*`, `std::env.*`, and `std::fs.*` work in `rhea run` (and `std::path`/`std::env` in `rhea vm`), but were not emitted in `NativeCodegen` (`rhea compile`) or `std::fs.*` in `BytecodeVM`.
  - Features that are genuinely interpreter-only or interpreter/VM-only (`try/catch` blocks, `std::ai.*` tensor calls, `pipeline` declarations, and untyped `std::json.parse` in `rhea compile`) are not caught by a pre-flight backend capability check before `BytecodeCompiler` or `NativeCodegen` runs.
- **v0.12.0 Engineering Solution**:
  - Add C++20 runtime support for `std::path.*`, `std::env.*`, and `std::fs.*` in `NativeCodegen` and `std::fs.*` in `BytecodeVM` so file, path, and environment operations work across all three backends (`run`, `vm`, `compile`).
  - Add `validate_backend_capabilities()` (`E2002`) before `rhea vm` and `rhea compile`/`emit-cpp` to detect any remaining backend-unsupported construct (`try/catch`, `std::ai.*`, `pipeline`, or `std::json.*` in `compile`) at its exact source `Span` and explain how to run the program with a supported backend.

### Gap 4: Diagnostic Precision & Levenshtein Suggestions Across Functions, Fields, Structs & CLI
- **Current State in `v0.11.0`**: Levenshtein suggestions are provided for variable names, but not for misspelled function calls (`prnt(1)`), misspelled struct names, misspelled struct field accesses (`pt.xx`), unknown `std::` modules (`import std::strng`), or misspelled CLI subcommands (`rhea rn app.rhea`). Furthermore, empty untyped list/map usage without type context can produce generic errors.
- **v0.12.0 Engineering Solution**:
  - Add edit-distance suggestions for functions, structs, struct fields, `std::*` modules, and CLI commands, improve parser delimiter/expression diagnostics, and translate external compiler failures into `E2003` diagnostics.

### Gap 5: Formatter (`rhea fmt`) Directory Mode & Multiline Consistency
- **Current State in `v0.11.0`**: `rhea fmt` only accepts a single file path (`rhea fmt file.rhea`) and fails if given a directory (`rhea fmt src/`) or invoked at a project root.
- **v0.12.0 Engineering Solution**:
  - Extend `rhea fmt` (`--check` and `-w`) to accept directories (recursively formatting all `.rhea` files in sorted order) or default to formatting `src/` and `tests/` when run in a project root with `rhea.toml`.

### Gap 6: Language Server (`rhea lsp`) Request IDs, Lifecycle Notifications, Error Codes & UTF-16 Positions
- **Current State in `v0.11.0`**:
  - `rhea lsp` hardcodes response IDs (`"id":1`, `"id":2`, etc.) instead of echoing the client's JSON-RPC request `"id"`.
  - `textDocument/didClose` and `textDocument/didSave` are not handled, and unknown JSON-RPC methods with an `"id"` do not return `-32601 Method not found` (or `-32700 Parse error` on malformed JSON).
  - Diagnostic and symbol character positions use 0-based UTF-8 byte offsets rather than LSP-standard 0-based UTF-16 code units.
- **v0.12.0 Engineering Solution**:
  - Echo exact JSON-RPC request IDs, handle `initialized`, `didSave`, `didClose` (clearing diagnostics), return `-32601`/`-32700` on unknown methods or malformed JSON, and convert 1-based UTF-8 byte columns to 0-based UTF-16 code unit offsets (`utf8_byte_col_to_utf16`).

### Gap 7: `rhea test` in Scaffolded User Projects (`rhea new` / `rhea init`)
- **Current State in `v0.11.0`**: `rhea test` hardcodes `python3 tests/run_all_tests.py`, which fails when a user runs `rhea test` inside a scaffolded project created by `rhea new my_app` (which contains `tests/test_main.rhea`).
- **v0.12.0 Engineering Solution**:
  - Make `rhea test` accept explicit `.rhea` test files/directories (`rhea test tests/test_main.rhea`) and automatically discover and execute `tests/*.rhea` when run inside a user project directory, while continuing to run `python3 tests/run_all_tests.py` when invoked in the compiler repository root.
