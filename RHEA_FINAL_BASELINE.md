# Rhea v0.13.0 — Phase 1 Final Baseline Audit (`RHEA_FINAL_BASELINE.md`)

## 1. Repository & Environment Baseline Inspection

### 1.1 Starting Repository State
- **Repository Root**: `.` (project root)
- **Reported Starting Version**: `Rhea Compiler & Runtime v0.12.0 (C ABI v2)`
- **Authoritative Implementation**:
  - `native/cpp/src/rhea_driver.cpp` (`8,163` lines of C++20): Lexer, Parser, ModuleResolver, TypeChecker, SimpleJsonParser, Tree-Walking Interpreter (`rhea run`), BytecodeCompiler & BytecodeVM (`rhea vm`), NativeCodegen (`rhea compile`), Formatter (`rhea fmt`), Language Server (`rhea lsp`), Debugger (`rhea debug`), Package Manager (`rhea pkg`), and CLI driver.
  - `native/cpp/src/kernels.cpp` + `native/cpp/include/rhea/rhea_c_api.h`: C++20 numerical, tensor, reverse-mode autodiff, Adam optimizer, and FNV-1a checkpoint library (`lib/librhea_native.so`, C ABI v2).
  - `python/rhea_ai/`: Python `ctypes` bindings for `librhea_native.so`.
  - `tests/differential_harness.py` (`28` cross-backend suites) and `tests/run_all_tests.py` (`145` automated tests).

### 1.2 Baseline Build, Sanitizer & Test Execution Results

Executed before modifying any file:

```text
$ make clean && make all && make test-asan
rm -rf bin/* lib/* build/*
c++ -std=c++20 -O3 -Wall -Wextra -Wpedantic -fPIC -Inative/cpp/include -dynamiclib -Wl,-install_name,@rpath/librhea_native.so native/cpp/src/kernels.cpp -o lib/librhea_native.so -lm
c++ -std=c++20 -O3 -Wall -Wextra -Wpedantic -fPIC -Inative/cpp/include native/cpp/src/rhea_driver.cpp -Llib -lrhea_native -Wl,-rpath,@loader_path/../lib -o bin/rhea -lm
c++ -std=c++20 -O3 -Wall -Wextra -Wpedantic -fPIC -Inative/cpp/include native/cpp/tests/test_kernels.cpp -Llib -lrhea_native -Wl,-rpath,@loader_path/../lib -o bin/rhea_cpp_tests -lm
c++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Inative/cpp/include native/cpp/src/kernels.cpp native/cpp/tests/test_kernels.cpp -o bin/rhea_cpp_tests_asan -lm
./bin/rhea_cpp_tests_asan
[PASS] All C++20 native kernel, autodiff, Adam, and checkpoint integrity tests passed.
c++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Inative/cpp/include native/cpp/src/rhea_driver.cpp -Llib -lrhea_native -Wl,-rpath,@loader_path/../lib -o bin/rhea_asan -lm
./bin/rhea_asan version
Rhea Compiler & Runtime v0.12.0 (C ABI v2)
```

```text
$ python3 tests/differential_harness.py
[Differential Harness] 28 cross-backend test suites passed (0 failures).
```

```text
$ ./bin/rhea test
Ran 145 tests in 90.415s — OK
```

---

## 2. Classification of Baseline Findings

### 2.1 Confirmed Defects Reproduced Locally (`D-13`–`D-20`)

| ID | Severity | Subsystem | Confirmed Defect & Local Reproduction | Planned Fix & Regression Test |
| :--- | :--- | :--- | :--- | :--- |
| **`D-13`** | **Critical (DoS / OOM)** | `Interpreter`, `BytecodeVM`, `NativeCodegen` (`std::string.split`) | Calling `std::string.split("abc", "")` with an empty delimiter loops infinitely (`s.find("", start)` returns `start`, `start += 0`) and allocates unbounded memory until process termination. | Handle `delim.empty()` deterministically across all 3 backends by splitting into individual UTF-8 character strings (`["a", "b", "c"]`, or `[]` for `""`). |
| **`D-14`** | **High (Crash / Overflow)** | `Lexer::tokenize` | 1) Float literals overflowing `double` (e.g., `1.0e500`) throw an uncaught `std::out_of_range`, printing `internal error: stod: out of range` without source span or error code.<br>2) Huge duration literals (`999999999999999999999999999999s`) call `std::llround(v * 1000.0)` without bounds checking, silently overflowing `int64_t`. | Catch `std::out_of_range`/`std::invalid_argument` on float/duration literals and validate finite bounds before casting to `int64_t`, emitting `E1002` with exact `Span`. |
| **`D-15`** | **High (Stack Overflow)** | `SimpleJsonParser` (`std::json.parse`) | `SimpleJsonParser::parse_value` has no recursion depth counter; deeply nested JSON arrays/objects (`[[[...]]]`) can overflow the C++ call stack (`SIGSEGV`). | Enforce `MAX_PARSE_DEPTH` (`256`) in `SimpleJsonParser::parse_value` and throw a clean `E1006` diagnostic when exceeded. |
| **`D-16`** | **High (Undefined Behavior)** | `checked_mod_i64` & `rhea_mod` | `checked_mod_i64(a, b)` and `NativeCodegen` `rhea_mod(a, b)` check `b == 0` but evaluate `a % b` unguarded when `a == INT64_MIN && b == -1`, which is C++20 undefined behavior (`[expr.mul]/4`, `SIGFPE` on x86_64). | Guard `if (a == std::numeric_limits<int64_t>::min() && b == -1) return 0;` in both `checked_mod_i64` and `rhea_mod`. |
| **`D-17`** | **High (Backend Miscompilation)** | `BytecodeCompiler`, `BytecodeVM`, `NativeCodegen` (`enum` & `match`) | `examples/07_enums_and_pattern_matching.rhea` passes in `rhea run`, fails at runtime in `rhea vm` (`Pattern match results: (), ()` $\rightarrow$ `E1005`), and fails C++ compilation in `rhea compile` (`unknown type name 'ResultCode'`). | Implement full `EnumDecl`, `Expr::Kind::EnumVariant`, and `Expr::Kind::Match` support in `TypeChecker`, `BytecodeCompiler`/`BytecodeVM`, and `NativeCodegen` for 3-backend parity. |
| **`D-18`** | **High (Backend Miscompilation)** | `NativeCodegen` (`try_parse_int` / `try_parse_float`) | `examples/11_interactive_number_guessing.rhea` passes in `rhea run` and `rhea vm`, but fails in `rhea compile` (`use of undeclared identifier 'try_parse_int'`). | Add `RheaParseInt` and `RheaParseFloat` C++ runtime structs and `rhea_try_parse_int` / `rhea_try_parse_float` helpers in `NativeCodegen` so `examples/11` compiles and runs natively. |
| **`D-19`** | **Medium (Codegen Collision & Warnings)** | `NativeCodegen` | 1) Valid Rhea identifiers that are C++ keywords (`template`, `default`, `class`, `namespace`, `new`, `delete`, `switch`, `case`, `operator`, `union`, `virtual`) fail `rhea compile` with `E2003`.<br>2) `if (a == b)` emits `if ((a == b))`, triggering Clang `-Wparentheses-equality` warnings on `stderr`. | Escape C++ reserved words in `NativeCodegen::sanitize_ident` and avoid double parentheses in `if`/`while` condition emission. |
| **`D-20`** | **Medium (Type Checker Gap)** | `TypeChecker` | `std::*` module function calls (e.g., `std::string.split(123, 456)`) and `EnumVariant` constructors were not checked for argument types/counts in `TypeChecker`. | Register `std::*` module function signatures and `EnumDecl` variant payload signatures in `TypeChecker` to reject invalid calls at compile time (`E0003`/`E0004`/`E0006`). |

### 2.2 Confirmed Performance Bottlenecks Measured Locally (`P-01`–`P-04`)

| ID | Subsystem | Baseline Measurement | Root Cause | Planned Optimization |
| :--- | :--- | :--- | :--- | :--- |
| **`P-01`** | `BytecodeCompiler` / `BytecodeVM` (`for i in a..=b`) | `1,903.63 ms` in `rhea vm` vs `8.10 ms` in `rhea run` for `for i in 1..=10000` (**235x slower**) | `Stmt::Kind::For` in `BytecodeCompiler` materializes a `10,000`-element `List<Int>` and copies that entire `std::vector<Value>` onto the stack twice per iteration ($O(N^2)$ copies). | Lower `for i in start..end` / `start..=end` in `BytecodeCompiler` directly to an $O(1)$-memory streaming integer counter loop (`cur <= end` / `cur < end`), matching `Interpreter` and `NativeCodegen`. |
| **`P-02`** | `Interpreter` & `BytecodeVM` (`push`, `IndexGet`, `FieldGet`, `len`) | `1,462.56 ms` (`vm`) and `257.80 ms` (`run`) for `5,000`-element list `push` + index sum | `LoadLocal xs` and `eval_expr(e->lhs)` copy the entire `std::vector<Value>` by value on every `push(xs, v)`, `xs[i]`, `obj.field`, and `len(xs)` call ($O(N^2)$ copies). | Mutate `locals` in-place for `push`/`pop`/`remove_key` and read `Ident` containers by `const Value*` reference on `Index`, `FieldAccess`, and `len` in both `Interpreter` and `BytecodeVM`. |
| **`P-03`** | `BytecodeVM` (`invoke_vm_fn`) | Copies `globals` `unordered_map` on every function call | `locals = globals;` allocates and copies hash buckets on every recursive function call (`8,361` copies in `fib(18)`). | Keep `globals` read-only after `__snapshot_globals` and fall back to `globals` on `LoadLocal` only when a symbol is not in `locals`. |
| **`P-04`** | `BytecodeChunk` & `BytecodeVM` (`LoadLocal` / `StoreLocal`) | String hashing (`std::unordered_map<std::string, Value>`) on every variable access | Every `LoadLocal` and `StoreLocal` hashes `ins.sym` (`std::string`). | Assign dense integer slot indices (`0 .. num_locals - 1`) in `BytecodeChunk` so `BytecodeVM` accesses local variables via flat `std::vector<Value>` indexing in $O(1)$ time. |

### 2.3 Existing Limitations That Are Intentional and Documented
- **`try / catch` Exception Handling**: Supported in `rhea run`; rejected upfront with `E2002` in `rhea vm` and `rhea compile`.
- **`std::ai.*` / `tensor()` Native C ABI Kernels**: Supported in `rhea run` and `python/rhea_ai`; rejected upfront with `E2002` in `rhea vm` and `rhea compile`.
- **`pipeline` Declarations**: Supported in `rhea run`; rejected upfront with `E2002` in `rhea vm` and `rhea compile`.
- **Dynamic Heterogeneous `std::json.*`**: Supported in `rhea run` and `rhea vm`; rejected upfront with `E2002` in `rhea compile`.

### 2.4 Environmental Blockers That Prevent Verification
- **Rust Workspace (`crates/rhea_*`)**: `cargo` and `rustc` are not installed on this macOS host (`command -v cargo rustc` returns exit code 1).
- **VS Code `.vsix` Extension Packaging (`tools/vscode-rhea/`)**: `vsce` is not installed on this macOS host (`command -v vsce` returns exit code 1); extension manifests, grammar, and icons are verified directly via Python/JSON/PNG tests.
