# Rhea Programming Language (`v0.13.1`)

<p align="center">
  <img src="assets/icons/rhea-logo.svg" width="128" height="128" alt="Rhea Programming Language Logo">
</p>

**Rhea** (`v0.13.1`) is an experimental, statically typed programming language and toolchain designed for readable general-purpose programming, systems scripting, and numerical/AI experimentation.

The authoritative implementation is written in **C++20** (`native/cpp/src/rhea_driver.cpp` and `native/cpp/src/kernels.cpp`) and provides three execution backends (**Tree-Walk Interpreter**, **Stack Bytecode VM**, and **Native C++20 AOT Code Generator**), a C ABI v2 numerical and reverse-mode autodiff library (`librhea_native`), a Python `ctypes` bridge (`python/rhea_ai`), and integrated developer tooling (`check`, `fmt`, `lint`, `debug`, `lsp`, `pkg`, `new`, `test`).

---

## 1. Project Maturity & Current Status

- **Current Version**: `v0.13.1` (`C ABI v2`)
- **Maturity**: **Experimental / Research & Educational Release**. The core C++20 compiler, type checker, three execution backends, CPU tensor/autodiff kernels, CLI tooling, and icon/editor integrations are implemented and verified by **165 automated tests** and **32 cross-backend differential conformance suites** (`96` total backend executions).
- **Important Scope Notes**:
  - The C++20 toolchain (`bin/rhea` and `lib/librhea_native.so`) is the authoritative implementation.
  - A 13-crate modular Rust workspace (`crates/rhea_*`) is included in the repository as an experimental port in progress; building it requires an external Rust toolchain (`cargo` / `rustc`) and it is not required to build or run `bin/rhea`.
  - Tensor and reverse-mode autodiff kernels execute deterministically on **CPU (`f64`)**. Physical GPU backends (`CUDA` / `Vulkan` / `Metal`) and direct LLVM IR generation are not implemented.

---

## 2. Verified Language Features

- **Ergonomic Static Typing & Default Immutability**:
  - Concise immutable bindings (`x = 10` or `let x: Int = 10`), explicit mutable bindings (`mut count = 0`), and compile-time/module constants (`const LIMIT = 100`).
  - Damerau-Levenshtein edit-distance typo protection (`E0004`) across variables, functions, structs, fields, `std::*` modules, and CLI commands.
- **Control Flow, Streaming Ranges & Durations**:
  - `if / else` statements and expressions, `while` loops, `for i in a..b` (exclusive) and `for i in a..=b` (inclusive) ranges (streaming in $O(1)$ memory across all three backends), and first-class duration literals (`500ms`, `1s`, `2.5s`).
- **Functions, Closures, Generics & Pipelines**:
  - Recursive functions, implicit final-expression returns, lexical value-snapshot closures (`|x: Int| x + offset`), pipeline operator (`|>`), generic functions (`fn id<T>(x: T) -> T`), and multi-parameter generic structs (`struct Pair<K, V>`).
- **Algebraic Enums, Structs & Pattern Matching**:
  - Value-semantic `struct` types, algebraic `enum` types with payloads (`Ok(Int)`, `Err(String)`), and `match` expressions verified across all three backends (`run`, `vm`, and `compile`).
- **Checked Arithmetic & Memory Safety Guards**:
  - Signed 64-bit integer overflow (`+`, `-`, `*`, `/`, unary `-`) traps deterministically with `E1002` across all three backends (`Interpreter`, `BytecodeVM`, and `NativeCodegen`).
  - Division and modulo by zero trap with `E1001`, and out-of-bounds list indexing or missing map keys trap with `E1003`.

---

## 3. Tested Code Example

Save the following program as `demo.rhea` (or run `examples/07_enums_and_pattern_matching.rhea` / `examples/13_modules_generics_closures.rhea`):

```rhea
enum Outcome {
    Ok(Int),
    Err(String)
}

fn eval_outcome(o: Outcome, scale: Int) -> Int {
    match o {
        Outcome::Ok(v) => v * scale,
        Outcome::Err(msg) => -1
    }
}

fn main() {
    factor = 3
    triple = |n: Int| n * factor

    mut sum = 0
    for i in 1..=5 {
        sum += triple(i)
    }

    res = eval_outcome(Outcome::Ok(14), 3)
    print("sum={sum}, outcome={res}")
}
```

Execute across all three backends:

```bash
./bin/rhea run demo.rhea
./bin/rhea vm demo.rhea
./bin/rhea compile demo.rhea -o build/demo && ./build/demo
```

All three backends print:

```text
sum=45, outcome=42
```

---

## 4. Supported Operating Systems & Dependencies

| Component | Supported Platforms | Required Dependencies |
|---|---|---|
| **Compiler, VM, Native Codegen & C ABI Runtime (`bin/rhea`, `lib/librhea_native.so`)** | **macOS** (`arm64` / `x86_64`) and **Linux** (`x86_64` / `aarch64`) | POSIX shell, `make`, a C++20 compiler (`clang++` 14+ or `g++` 11+ accessible as `c++`), and `python3` (3.9+) |
| **Python AI Bridge (`python/rhea_ai`)** | **macOS** and **Linux** | `python3` (`ctypes` standard library; no third-party Python packages required) |
| **Editor & Desktop Icon Integrations (`tools/vscode-rhea/`, `tools/desktop/`)** | **VS Code** (cross-platform), **macOS** (`Info.plist` / `.icns`), **Windows** (`.reg` / `.ps1` / `.ico`), **Linux** (`shared-mime-info` / `hicolor`) | Standard OS desktop utilities (`plutil`/`lsregister` on macOS, PowerShell/`regedit` on Windows, `update-mime-database` on Linux) |

---

## 5. Building from Source

Clone the repository into any directory and run `make all`:

```bash
git clone https://github.com/yousefppp656-collab/Rhea.git
cd Rhea
make clean && make all
./bin/rhea version
# Output: Rhea Compiler & Runtime v0.13.1 (C ABI v2)
```

You can customize the compiler or output directories via Make variables:

```bash
make CXX=clang++ BIN_DIR=bin LIB_DIR=lib BUILD_DIR=build all
```

---

## 6. Executing Programs & CLI Tooling

| Command | Description |
|---|---|
| `./bin/rhea run <file.rhea>` | Type-check and execute `<file.rhea>` in the Tree-Walk Interpreter |
| `./bin/rhea vm <file.rhea>` | Compile to stack bytecode, run the bytecode verifier, and execute in the Bytecode VM |
| `./bin/rhea compile <file.rhea> -o <out>` | Emit standalone C++20 source and compile a native binary via `c++ -std=c++20 -O2` |
| `./bin/rhea check <file.rhea>` | Run lexical, syntactic, module, and static type validation without executing |
| `./bin/rhea fmt [--check \| -w] <path>` | Format a `.rhea` file or directory deterministically |
| `./bin/rhea lint <file.rhea>` | Run static lint checks |
| `./bin/rhea disasm <file.rhea>` | Disassemble verified stack bytecode instructions |
| `./bin/rhea emit-cpp <file.rhea>` | Print generated C++20 source to `stdout` |
| `./bin/rhea debug <file.rhea>` | Run the interactive CLI debugger (`break`, `step`, `continue`, `locals`, `stack`, `eval`) |
| `./bin/rhea lsp` | Start the JSON-RPC 2.0 Language Server over `stdin`/`stdout` |
| `./bin/rhea new <dir>` | Scaffold a new Rhea project (`rhea.toml`, `src/main.rhea`, `tests/test_main.rhea`) |
| `./bin/rhea pkg <lock \| verify>` | Generate or verify SHA-256 package lockfiles (`rhea.toml` & `rhea.lock`) |

---

## 7. Running the Test, Sanitizer & Validation Suites

```bash
# 1. Run C++20 native kernel tests + all 165 end-to-end automated tests
make test
# or directly:
./bin/rhea test

# 2. Run the 32-suite (96-execution) cross-backend differential conformance harness
python3 tests/differential_harness.py

# 3. Build and run AddressSanitizer + UndefinedBehaviorSanitizer checks
make test-asan

# 4. Regenerate and validate the multi-platform SVG, PNG, ICNS, ICO & VS Code icon suite
python3 tools/generate_icons.py
python3 tools/validate_icons.py

# 5. Run the cross-backend performance benchmark suite
python3 benchmarks/run_benchmarks.py
```

---

## 8. Backend Capabilities & Known Limitations

Rhea provides three execution backends with verified semantic parity across 32 core language suites. Specific dynamic or runtime-coupled features are intentionally scoped to specific backends and fail upfront with `E2002` where unsupported (see [**`RHEA_BACKEND_CAPABILITIES.md`**](RHEA_BACKEND_CAPABILITIES.md)):

| Feature | Interpreter (`rhea run`) | Bytecode VM (`rhea vm`) | Native Compiler (`rhea compile`) |
|---|---|---|---|
| Core types, checked 64-bit arithmetic, strings, lists, maps, structs, generics, closures, modules | Supported & tested | Supported & tested | Supported & tested |
| Algebraic `enum` with payloads & `match` | Supported & tested | Supported & tested | Supported & tested |
| Standard library (`math`, `string`, `io`, `fs`, `path`, `env`, `time`, `log`, `test`) | Supported & tested | Supported & tested | Supported & tested |
| Dynamic untyped `std::json.parse` / `stringify` | Supported & tested | Supported & tested | Rejected upfront (`E2002`) |
| `try / catch` exception blocks & `pipeline` declarations | Supported & tested | Rejected upfront (`E2002`) | Rejected upfront (`E2002`) |
| Native C ABI tensor & autodiff (`std::ai.*` / `tensor`) | Supported & tested | Rejected upfront (`E2002`) | Rejected upfront (`E2002`) |

Additional known limitations:
- Closures capture outer variables by **lexical value snapshot** at creation time; mutating an outer variable inside a closure is rejected at compile time with `E0005`.
- Native compilation (`rhea compile`) invokes the host C++20 compiler (`$RHEA_CXX`, `$CXX`, or `c++`) and targets POSIX platforms (`macOS` and `Linux`).

---

## 9. Documentation, Examples & Release Archives

- **Language & Developer Docs**:
  - [Language Specification (`docs/LANGUAGE_SPECIFICATION.md`)](docs/LANGUAGE_SPECIFICATION.md)
  - [Standard Library Reference (`docs/STANDARD_LIBRARY.md`)](docs/STANDARD_LIBRARY.md)
  - [Developer Guide (`RHEA_DEVELOPER_GUIDE.md`)](RHEA_DEVELOPER_GUIDE.md)
  - [Diagnostics Reference (`RHEA_DIAGNOSTICS_GUIDE.md`)](RHEA_DIAGNOSTICS_GUIDE.md)
  - [Backend Capabilities Matrix (`RHEA_BACKEND_CAPABILITIES.md`)](RHEA_BACKEND_CAPABILITIES.md)
  - [Brand & Icon Integration Guide (`RHEA_ICON_GUIDE.md`)](RHEA_ICON_GUIDE.md)
  - [Performance Benchmarks (`RHEA_BENCHMARKS.md`)](RHEA_BENCHMARKS.md)
  - [Security & Reliability Audit (`RHEA_SECURITY_AUDIT.md`)](RHEA_SECURITY_AUDIT.md)
  - [Publication & Clean-Checkout Verification (`PUBLICATION_VERIFICATION.md`)](PUBLICATION_VERIFICATION.md)
- **Examples**:
  - `examples/01_fibonacci_and_primes.rhea` through `examples/15_json_config_and_collections.rhea`
  - `examples/readability/01_hello_world.rhea` through `examples/readability/10_word_frequency.rhea`
- **Release Package**:
  - [`release/Rhea_v0.13.1_Release.zip`](release/Rhea_v0.13.1_Release.zip) and [`release/Rhea_v0.13.1_Release.zip.sha256`](release/Rhea_v0.13.1_Release.zip.sha256)
  - [`release/RELEASE_NOTES_v0.13.1.md`](release/RELEASE_NOTES_v0.13.1.md)

---

## 10. Roadmap (Existing vs. Planned Work)

- **Implemented & Verified in `v0.13.1`**:
  - Statically typed frontend with Levenshtein diagnostics (`E0001`–`E3002`), local modules, generics, closures, algebraic enums with `match`, three execution backends (`run`, `vm`, `compile`), CPU `f64` tensor/autodiff C ABI v2 library, CLI developer tools (`fmt`, `lint`, `debug`, `lsp`, `pkg`, `new`), and multi-platform `.rhea` icon integrations.
- **Planned Future Work (`v0.14+` / `v1.0`)**:
  - Full `std::ai` tensor linking in standalone `rhea compile` binaries and `rhea vm`.
  - Typed schema-driven JSON serialization in `rhea compile`.
  - Completing and verifying the standalone Rust workspace (`crates/rhea_*`).
  - Optional hardware acceleration backends (SIMD / BLAS / GPU) for `librhea_native`.

---

## 11. Contributing, Security & License

- **Contributing**: See [CONTRIBUTING.md](CONTRIBUTING.md) and [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).
- **Reporting Bugs & Security Issues**: Use the GitHub Issue Templates in `.github/ISSUE_TEMPLATE/` for bug reports and feature requests, and follow [SECURITY.md](SECURITY.md) (via GitHub Private Vulnerability Reporting) for security vulnerabilities.
- **License**: Licensed under the **MIT License** (`Copyright (c) 2026 Rhea Language Contributors`). See [LICENSE](LICENSE) for full terms.
