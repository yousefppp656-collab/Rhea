# Rhea v0.13.0 — Security & Runtime Reliability Audit (`RHEA_SECURITY_AUDIT.md`)

## 1. Scope & Methodology

This security and runtime reliability audit covers the C++20 compiler, lexer, recursive-descent parser, module resolver, static type checker, JSON parser, tree-walking interpreter, stack bytecode compiler/verifier/VM, native C++20 code generator, C ABI v2 tensor/autodiff runtime (`librhea_native`), Language Server (`rhea lsp`), and CLI/package tooling in the Rhea repository.

All findings were investigated against the actual source code (`native/cpp/src/rhea_driver.cpp` and `native/cpp/src/kernels.cpp`) and verified using Apple Clang (`-std=c++20 -Wall -Wextra -Wpedantic`) with **AddressSanitizer (`-fsanitize=address`)** and **UndefinedBehaviorSanitizer (`-fsanitize=undefined`)** enabled (`bin/rhea_asan` and `bin/rhea_cpp_tests_asan`).

---

## 2. Confirmed & Remediated Security / Reliability Defects

### 2.1 Critical & High-Severity Defects Fixed in `v0.13.0` (Phase 2)

| ID | Severity | Vulnerability / Reliability Class | Root Cause in `v0.12.0` | Fix Applied in `v0.13.0` | Verification Test |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`D-13`** | **Critical** | Denial of Service (Infinite Loop & Unbounded Memory Allocation / OOM) | `std::string.split(s, "")` called `s.find("", start)` which returned `start` (`0`) and incremented `start += delim.size()` (`0`), looping forever and allocating infinite substrings across `Interpreter`, `BytecodeVM`, and `NativeCodegen`. | Implemented `split_string_values(s, delim)` and updated `rhea_fn_split` in `NativeCodegen` to split `s` into individual UTF-8 code points in bounded $O(|s|)$ time when `delim.empty()`. | `test_146`, Differential Suite `30` |
| **`D-14`** | **High** | Uncaught `std::out_of_range` Exception & Float-to-Int Undefined Behavior in Lexer | 1) Float literals overflowing IEEE-754 `double` (`1.0e500`) threw an uncaught `std::out_of_range` in `Lexer::tokenize`.<br>2) Huge duration literals (`999999999999999999999999999999s`) called `std::llround(v * 1000.0)` without checking finite/64-bit bounds, triggering float-cast-overflow UB.<br>3) `-9223372036854775808` (`INT64_MIN`) failed `std::stoll` before unary `-` was applied. | Wrapped `std::stod` in `try/catch`, validated `std::isfinite` and millisecond bounds `[-9.0e15 .. 9.0e15]` before `std::llround` (`E1002`), and added exact `-9223372036854775808` (`INT64_MIN`) token folding. | `test_147`, `test_148`, Differential Suite `31` |
| **`D-15`** | **High** | Call-Stack Exhaustion (`SIGSEGV`) via Deeply Nested JSON | `SimpleJsonParser::parse_val` recursed on nested `[` and `{` without a depth counter, allowing deeply nested JSON inputs to exhaust the C++ call stack. | Added RAII `DepthGuard` enforcing `MAX_PARSE_DEPTH = 256` in `SimpleJsonParser::parse_val`, raising `E1006` cleanly when exceeded. | `test_149` |
| **`D-16`** | **High** | Signed Integer Division/Modulo & Negation Overflow (`INT64_MIN`) | `INT64_MIN / -1`, `INT64_MIN % -1`, and `-INT64_MIN` are undefined behavior in C++20 (`[expr.mul]/4`). | Guarded `a == INT64_MIN && b == -1` in `checked_div_i64`/`rhea_div` (`E1002`), `checked_mod_i64`/`rhea_mod` (returns `0`), and `checked_sub_i64(0, a)`/`rhea_neg` (`E1002`) across all 3 backends. | `test_148`, Differential Suite `31` |
| **`LSP-01`** | **Medium** | Unbounded Allocation via Malformed `Content-Length` Header in `rhea lsp` | `run_lsp_server` allocated `std::string body(len, '\0')` directly from the parsed `Content-Length` header without an upper bound. | Capped `Content-Length` at `16 MB` (`16 * 1024 * 1024` bytes), returning JSON-RPC error `-32600` (`Invalid Content-Length`) on zero or oversized headers, and verified `std::cin.gcount() == len`. | `test_155` |

### 2.2 Earlier Hardening Verified & Preserved (`v0.10.0`–`v0.12.0`)

| Area | Security / Safety Control | Enforced Limit / Mechanism | Verification Test |
| :--- | :--- | :--- | :--- |
| **Native Compiler Invocation (`rhea compile`)** | Command-injection prevention on filenames with spaces or shell metacharacters | Uses direct POSIX `fork()` + `execvp()` argument arrays (`run_compiler_process`); never invokes `/bin/sh` or `std::system`. | `test_27`, `test_156` |
| **C++20 String Literal Codegen** | Code-injection prevention in generated C++ source | `NativeCodegen::escape_cpp_string` escapes `\\`, `"`, `\n`, `\r`, `\t`, and non-printable bytes as `\xHH`. | `test_27` |
| **C++20 Identifier Sanitization** | Prevention of C++ keyword collisions in generated source | `NativeCodegen::cpp_ident` escapes all 88 C++20 reserved words with `_rhea_kw`. | `test_152`, Differential Suite `30` |
| **Nested Closure Compilation (`BytecodeCompiler`)** | Prevention of vector reallocation use-after-free | `BytecodeCompiler::compile` copies `ExprPtr lam = pending_lambdas_[i]` by value so nested lambdas pushing to `pending_lambdas_` never invalidate `lam`. | `test_129` (under ASan) |
| **`SimpleJsonParser` String Key Lifetime** | Prevention of dangling `std::string` reference | `SimpleJsonParser::parse_obj` stores `std::string k = parse_str()` in a local variable before calling `parse_val()`. | `test_88`, `test_118` (under ASan) |
| **Parser Nesting Guard** | Call-stack overflow prevention on deeply nested expressions/blocks | `MAX_PARSE_DEPTH = 256` enforced in `Parser::parse_expr` and `Parser::parse_block` (`E0002`). | `test_107`, `test_116` |
| **Runtime Call-Stack & VM Step Guards** | Infinite recursion & infinite VM loop protection | `MAX_RECURSION_DEPTH = 256` and `MAX_VM_STEPS = 50,000,000` (`E1004`). | `test_21` |
| **Range Materialization Guard** | Memory exhaustion prevention on materialized ranges (`a..=b`) | `MAX_RANGE_ELEMENTS = 10,000,000` (`E1004`), while `for i in a..=b` streams in $O(1)$ memory across all 3 backends. | `test_72`, `test_108` |
| **Module & Project Path Traversal Guard** | Directory traversal prevention in `import` and `rhea new` | Rejects `..` parent traversal segments and absolute paths in `ModuleResolver` (`E0009`) and `rhea new` (`E3001`). | `test_69`, `test_87` |
| **Native Tensor & Checkpoint Integrity (`librhea_native`)** | Overflow-checked tensor allocation & FNV-1a checkpoint verification | `safe_product` caps tensor size at `MAX_TENSOR_ELEMENTS` (`64M` elements = `512 MB`); `rhea_mlp_load_checkpoint` validates 64-bit FNV-1a checksum and `std::isfinite` on all weights. | `bin/rhea_cpp_tests_asan`, `test_30` |

---

## 3. Sanitizer & Fuzzing Verification Summary

- **AddressSanitizer + UndefinedBehaviorSanitizer (`make test-asan`)**:
  - `bin/rhea_cpp_tests_asan`: **PASS** (`0` memory leaks, `0` out-of-bounds accesses, `0` undefined behavior).
  - `bin/rhea_asan`: **PASS** across compiler, VM, formatter, nested closures, and boundary arithmetic (`test_118`, `test_158`).
- **Bounded Syntactic & Semantic Fuzzing (`test_116`, `test_117`)**:
  - `40` seeded adversarial/malformed token streams and `25` invalid semantic programs tested per run with explicit size and timeout bounds (`0` crashes, `0` hangs).
