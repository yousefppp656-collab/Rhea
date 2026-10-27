# Rhea v0.13.0 — Backend Capabilities & Conformance Classification

Rhea provides three execution backends compiled into the `bin/rhea` driver:
1. **`rhea run` (Tree-Walking AST Interpreter)** — Full reference implementation supporting 100% of the Rhea language, standard library, interactive I/O, exception handling (`try`/`catch`), declarative pipelines (`pipeline`), and native C++20 AI/tensor kernels (`std::ai`).
2. **`rhea vm` (Stack-Based Bytecode Virtual Machine)** — Compiles Rhea AST into verified stack bytecode (`BytecodeChunk`) with lexical `CallFrame` activation records, closures, recursion, algebraic `enum`/`match` pattern matching, nested lvalue mutation, streaming $O(1)$ range loops, and `std::json` support.
3. **`rhea compile` / `rhea build` (Native C++20 Ahead-of-Time Compiler)** — Lowers statically typed Rhea programs into standalone C++20 source and invokes the system `c++` compiler via safe POSIX `fork`/`execvp` to produce native Mach-O/ELF executables.

---

## 1. Classification Legend

Every language feature, standard library module, and developer tool below is classified using one of five explicit verification categories:
- **`Supported and tested`** — Implemented, verified by automated regression and differential tests in `v0.13.0`.
- **`Supported with documented restrictions`** — Implemented and tested within documented static or architectural constraints.
- **`Experimental`** — Present for exploration or specialized workflows; not part of the cross-backend conformance guarantee.
- **`Unsupported`** — Not supported by that specific backend; rejected upfront at compile time with diagnostic **`E2002`** (`Unsupported Backend Capability`) and an actionable recommendation.
- **`Blocked from verification by missing tooling`** — Source code exists in the repository, but verification could not be executed on this machine due to absent external host compilers/packagers (`rustc`/`cargo`, `vsce`).

---

## 2. Core Language Feature Matrix Across Backends (`32/32` Differential Suites = `96` Executions)

| Feature / Construct | `rhea run` (Interpreter) | `rhea vm` (Bytecode VM) | `rhea compile` (Native C++20) | Verification & Notes |
| :--- | :--- | :--- | :--- | :--- |
| `Int` (incl. `INT64_MIN` literal `-9223372036854775808`), `Float`, `Bool`, `String`, `Unit` | `Supported and tested` | `Supported and tested` | `Supported and tested` | Differential Suites 01–32 (`test_148`) |
| Checked 64-bit `Int` overflow (`E1002`) | `Supported and tested` | `Supported and tested` | `Supported and tested` | `+`, `-`, `*`, `INT64_MIN / -1`, unary `-INT64_MIN` trap with `E1002`; `INT64_MIN % -1 == 0` guarded (Suites 15, 31, Tests 96, 148) |
| Division & modulo by zero (`E1001`) | `Supported and tested` | `Supported and tested` | `Supported and tested` | `Int` and `Float` `/`, `%` trap with `E1001` (Suite 16, Test 97) |
| Arithmetic precedence & `&&`/`\|\|` short-circuit | `Supported and tested` | `Supported and tested` | `Supported and tested` | Suite 14 & Property Test 111 |
| String ordering (`<`, `<=`, `>`, `>=`) & interpolation | `Supported and tested` | `Supported and tested` | `Supported and tested` | Lexicographical byte order (Suite 18, Test 99) |
| Variable bindings (`x = 1`, `let`, `mut`, `const`) & C++ keyword identifiers | `Supported and tested` | `Supported and tested` | `Supported and tested` | Top-level `const` emitted at file scope; C++20 keywords escaped via `cpp_ident` (Suites 17, 30, Test 152) |
| Nested block scoping & lexical shadowing | `Supported and tested` | `Supported and tested` | `Supported and tested` | Scope-qualified SSA renaming (`name$sN`) (Suite 01, Test 58) |
| `if`/`else`, `while`, `for`, `break`, `continue` | `Supported and tested` | `Supported and tested` | `Supported and tested` | Suites 05, 06 & Property Test 112 |
| Inclusive (`..=`) & exclusive (`..`) ranges | `Supported and tested` | `Supported and tested` | `Supported and tested` | Streamed in $O(1)$ memory in `for` loops across all 3 backends; safe at `INT64_MAX..=INT64_MAX` (Suites 05, 22, Test 157) |
| Duration literals (`500ms`, `1.5s`) | `Supported and tested` | `Supported and tested` | `Supported and tested` | Lowered to `Int` milliseconds with overflow guard `E1002` (Tests 51, 147) |
| Functions, mutual recursion & implicit final expression return | `Supported and tested` | `Supported and tested` | `Supported and tested` | Suites 02, 23 (`main() -> Int` exit code propagation) |
| Closures (`\|x: Int\| expr` and multi-statement `{ ... }`) | `Supported and tested` | `Supported and tested` | `Supported and tested` | Immutable lexical value-snapshot capture; local `mut` bindings & nested closures supported; outer capture mutation rejected with `E0005` (Suite 03, Tests 128–129) |
| Generic functions (`fn f<T, U>`) & generic structs (`struct S<K, V>`) | `Supported and tested` | `Supported and tested` | `Supported and tested` | Multi-parameter type unification & body operator checking (Suites 04, 20, Test 104) |
| Struct declarations, field access, `==`/`!=`, `print` | `Supported and tested` | `Supported and tested` | `Supported and tested` | Canonical declaration field order on init (Suite 19, Test 102) |
| **Algebraic `enum` with payloads & `match` pattern matching** | `Supported and tested` | `Supported and tested` | `Supported and tested` | Upgraded to full 3-backend parity in `v0.13.0` (`ResultCode::Ok(21)`, `ResultCode::Err("timeout")`, `match`) (Suite 28, Test 150) |
| Homogeneous `List<T>` & `Map<String, V>` | `Supported and tested` | `Supported and tested` | `Supported and tested` | Bounds & missing-key trap with `E1003` (Suites 08–12, 21) |
| **Nested Lvalue Mutation** (`matrix[i][j] = v`, `obj.items[i] += v`, `obj.inner.field = v`) | `Supported and tested` | `Supported and tested` | `Supported and tested` | Left-to-right single index evaluation (`R:C:`), root `mut` check (`E0005`), leaf type check (`E0003`), bounds check (`E1003`) (Suites 24–26, Tests 123–127) |
| Local multi-file modules (`import math_utils`, `pub fn`, `priv fn`) | `Supported and tested` | `Supported and tested` | `Supported and tested` | Circular import, path traversal & cross-module symbol collision checks (`E0009`, `E0002`) (Suite 32, Tests 68–70, 119) |
| `try { ... } catch err { ... }` exception handling | `Supported and tested` | `Unsupported` (`E2002`) | `Unsupported` (`E2002`) | Pre-flight validator emits `E2002` in `vm`/`compile` and directs user to `rhea run` (Test 130) |
| `pipeline` declarations | `Supported and tested` | `Unsupported` (`E2002`) | `Unsupported` (`E2002`) | Pre-flight validator emits `E2002` in `vm`/`compile` and directs user to `rhea run` |

---

## 3. Standard Library Module Support Across Backends

| Module | Key Functions | `rhea run` | `rhea vm` | `rhea compile` | Verification & Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `std::io` | `print`, `println`, `print_inline`, `eprintln`, `input`, `read_line`, `is_eof`, `try_parse_int`, `try_parse_float`, `parse_int`, `parse_float` | `Supported and tested` | `Supported and tested` | `Supported and tested` | Upgraded `try_parse_int` & `try_parse_float` to full 3-backend parity in `v0.13.0` (Suites 07, 13, 29, Test 151) |
| `std::math` | `abs`, `min`, `max`, `clamp`, `sqrt`, `sin`, `cos`, `exp`, `log`, `pow`, `floor`, `ceil`, `round` | `Supported and tested` | `Supported and tested` | `Supported and tested` | Checked across all 3 backends |
| `std::string` | `len`, `char_count`, `trim`, `upper`, `lower`, `split`, `join`, `replace`, `contains`, `starts_with`, `ends_with` | `Supported and tested` | `Supported and tested` | `Supported and tested` | UTF-8 `char_count` and UTF-8 empty-delimiter `split(s, "")` across all 3 backends (Suite 30, Tests 90, 146) |
| `std::collections` | `len`, `push`, `pop`, `contains`, `reverse`, `sort`, `has_key`, `keys`, `values`, `remove_key` | `Supported and tested` | `Supported and tested` | `Supported and tested` | Suites 09, 12, 21, Test 103 |
| `std::time` | `sleep`, `sleep_ms`, `now_ms`, `monotonic_ms` | `Supported and tested` | `Supported and tested` | `Supported and tested` | Real monotonic clock & thread sleep (Tests 51–52) |
| `std::path` | `join`, `basename`, `dirname`, `extension`, `stem`, `is_abs` | `Supported and tested` | `Supported and tested` | `Supported and tested` | Full 3-backend parity + static type checking (Suite 27, Tests 133, 153) |
| `std::env` | `get`, `set`, `has` | `Supported and tested` | `Supported and tested` | `Supported and tested` | Full 3-backend parity (Suite 27, Test 133) |
| `std::fs` | `exists`, `read_text`, `read_lines`, `write_text`, `append_text`, `remove_file`, `mkdir_p` | `Supported and tested` | `Supported and tested` | `Supported and tested` | Full 3-backend parity (Suite 27, Tests 92, 133) |
| `std::log` | `info`, `warn`, `error` | `Supported and tested` | `Supported and tested` | `Supported and tested` | Tests 93, 109 |
| `std::json` | `parse`, `stringify` | `Supported and tested` | `Supported and tested` | `Unsupported` (`E2002`) | `std::json.parse` returns dynamic heterogeneous trees (`MAX_PARSE_DEPTH = 256`); `rhea compile` emits `E2002` (Tests 88–89, 132, 149) |
| `std::ai` | `tensor`, `matmul`, `relu`, `mse_loss`, `backward`, `adam_step`, `train_xor_demo`, `save_checkpoint`, `load_checkpoint` | `Supported and tested` | `Unsupported` (`E2002`) | `Unsupported` (`E2002`) | Backed by `librhea_native` C ABI v2 in `rhea run` and `python/rhea_ai`; `vm` and `compile` emit `E2002` (Tests 05, 08, 22–23, 131) |

---

## 4. Developer Tooling & Host Environment Matrix

| Component | Classification | Verification Evidence |
| :--- | :--- | :--- |
| `bin/rhea` CLI (`run`, `vm`, `check`, `compile`, `build`, `fmt`, `lint`, `lsp`, `debug`, `test`, `new`, `init`, `pkg`, `disasm`, `emit-cpp`) | `Supported and tested` | Verified across `160/160` automated tests (`tests/run_all_tests.py`) and `32/32` differential suites (`96` executions in `tests/differential_harness.py`) |
| `bin/rhea_asan` & `bin/rhea_cpp_tests_asan` (AddressSanitizer + UndefinedBehaviorSanitizer) | `Supported and tested` | Compiled and executed with `-fsanitize=address,undefined` (`0` warnings, `0` errors, `0` sanitizer findings) |
| `python/rhea_ai` ctypes bridge (`librhea_native.so`, C ABI v2) | `Supported and tested` | Verified by `tests/run_all_tests.py` (`test_22`, `test_23`, `test_30`) |
| `crates/rhea_*` (`rhea-compiler` 13-crate Rust workspace) | `Blocked from verification by missing tooling` | `rustc` and `cargo` are not installed in this macOS environment (`command -v cargo rustc` returns exit code 1) |
| `tools/vscode-rhea/` `.vsix` extension packaging | `Blocked from verification by missing tooling` | JSON manifests, TextMate grammar, and icon assets are validated by `test_94`, but `vsce` is not installed |
