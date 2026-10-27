# Rhea v0.13.0 — Performance Engineering & Benchmark Report (`RHEA_BENCHMARKS.md`)

## 1. Benchmark Methodology & Environment

- **Machine & OS**: macOS (`Darwin arm64`)
- **Compiler & Flags**: Apple Clang (`c++ -std=c++20 -O3 -Wall -Wextra -Wpedantic -fPIC`)
- **Harness**: [`benchmarks/run_benchmarks.py`](benchmarks/run_benchmarks.py)
- **Measurement Protocol**:
  - Each workload executes **1 warm-up trial** followed by **3 timed trials** (5 trials for standalone native binaries and CLI startup) using `time.perf_counter()`.
  - Every trial verifies that the process exits with code `0` and produces the exact expected mathematical output on `stdout`.
  - All timings include full process startup, lexing, parsing, static type checking, and execution for `rhea run` and `rhea vm`.

---

## 2. Measured Performance Before (`v0.12.0`) vs. After (`v0.13.0`)

### 2.1 Bytecode VM (`rhea vm`) — Before vs. After

| Workload | Description | `v0.12.0` Baseline (Mean ± SD) | `v0.13.0` Optimized (Mean ± SD) | Measured Speedup |
| :--- | :--- | :--- | :--- | :--- |
| **`W1_range_arith_10k`** | `10,000`-iteration `for i in 1..=10000` range loop with checked integer arithmetic | `1,909.73 ± 28.4 ms` | **`7.42 ± 0.19 ms`** | **`257.38x` faster** |
| **`W2_recursion_fib20`** | Recursive `fib(20)` (`21,891` function calls) | `9.81 ± 0.31 ms` | **`9.90 ± 0.22 ms`** | `1.00x` (already fast; zero regression) |
| **`W3_list_push_and_index_5k`** | `5,000` `push(xs, i)` calls + `5,000` indexed reads `xs[i]` | `1,477.54 ± 19.8 ms` | **`7.25 ± 0.15 ms`** | **`203.80x` faster** |
| **`W4_struct_field_loop_5k`** | `5,000`-iteration loop mutating and reading struct fields (`c.x += i; c.y += c.x % 7`) | `500.79 ± 11.2 ms` | **`7.49 ± 0.24 ms`** | **`66.86x` faster** |
| **`W5_generics_and_strings_2k`** | `2,000` generic `identity<T>` calls + `std::string.replace`, `split`, `join`, `len` | `89.28 ± 2.1 ms` | **`6.98 ± 0.18 ms`** | **`12.79x` faster** |

### 2.2 Tree-Walking AST Interpreter (`rhea run`) — Before vs. After

| Workload | Description | `v0.12.0` Baseline (Mean ± SD) | `v0.13.0` Optimized (Mean ± SD) | Measured Speedup |
| :--- | :--- | :--- | :--- | :--- |
| **`W1_range_arith_10k`** | `10,000`-iteration `for i in 1..=10000` range loop | `5.91 ± 0.14 ms` | **`6.20 ± 0.18 ms`** | `0.95x` (within 0.3 ms OS spawn noise; streaming $O(1)$ range) |
| **`W2_recursion_fib20`** | Recursive `fib(20)` (`21,891` function calls) | `281.87 ± 6.4 ms` | **`19.01 ± 0.45 ms`** | **`14.83x` faster** |
| **`W3_list_push_and_index_5k`** | `5,000` `push(xs, i)` calls + `5,000` indexed reads `xs[i]` | `243.58 ± 5.1 ms` | **`6.61 ± 0.16 ms`** | **`36.85x` faster** |
| **`W4_struct_field_loop_5k`** | `5,000`-iteration struct field mutation & read loop | `7.97 ± 0.21 ms` | **`7.21 ± 0.19 ms`** | **`1.11x` faster** |
| **`W5_generics_and_strings_2k`** | `2,000` generic calls + `std::string` operations | `29.19 ± 0.85 ms` | **`10.66 ± 0.31 ms`** | **`2.74x` faster** |

### 2.3 Native C++20 Compiled Binary (`rhea compile`) & Compilation Time

| Workload | Native Binary Execution (`v0.13.0`) | AOT Compile Time (`rhea compile -O3`) | Ground-Truth Output Verified |
| :--- | :--- | :--- | :--- |
| **`W1_range_arith_10k`** | `6.27 ms` | `558.17 ms` | `149985002` |
| **`W2_recursion_fib20`** | `6.40 ms` | `556.94 ms` | `6765` |
| **`W3_list_push_and_index_5k`** | `6.58 ms` | `575.96 ms` | `12497500` |
| **`W4_struct_field_loop_5k`** | `6.41 ms` | `558.32 ms` | `12512505` |
| **`W5_generics_and_strings_2k`** | `7.31 ms` | `624.44 ms` | `34000` |

---

## 3. Engineering Root-Cause Analysis of Optimizations (`P-01`–`P-03`)

1. **`P-01`: Streaming $O(1)$ Range Loops in `BytecodeCompiler`/`BytecodeVM`, `Interpreter`, and `NativeCodegen`**:
   - **Bottleneck**: In `v0.12.0`, `BytecodeCompiler` compiled `for i in 1..=10000` by materializing a `10,000`-element `std::vector<Value>` via `RANGE_INCL` and then executing `LoadLocal __for_list` twice per iteration (copying all `10,000` elements `20,000` times = $2 \times 10^8$ `Value` copies).
   - **Optimization**: `BytecodeCompiler` now lowers `for i in start..end` and `for i in start..=end` directly to a streaming integer counter loop (`__range_iter_next` with `INT64_MAX` boundary protection), eliminating all list allocations and vector copies (`1,909.73 ms` $\rightarrow$ `7.42 ms`, **`257.38x` speedup**). `Interpreter` and `NativeCodegen` similarly stream range bounds in $O(1)$ memory without per-iteration scope or vector allocation.
2. **`P-02`: Zero-Copy Local Container Operations (`__push_local`, `__pop_local`, `__index_local`, `__field_local`, `__len_local`)**:
   - **Bottleneck**: In `v0.12.0`, `push(xs, i)` in `BytecodeVM` copied `xs` onto the stack, appended `i`, and stored `xs` back (`O(N)` per `push` = $O(N^2)$ overall), and `xs[i]` copied `xs` onto the stack in both `BytecodeVM` and `Interpreter`.
   - **Optimization**: Added in-place local container mutation and `const Value*` reference lookup for `Ident` containers in both `BytecodeVM` and `Interpreter`, reducing `W3_list_push_and_index_5k` from `1,477.54 ms` to `7.25 ms` (**`203.80x` speedup**) in `rhea vm` and from `243.58 ms` to `6.61 ms` (**`36.85x` speedup**) in `rhea run`.
3. **`P-03`: Exception-Free `has_return_` Signaling & Read-Only Global Scope Fallback in `Interpreter` and `BytecodeVM`**:
   - **Bottleneck**: In `v0.12.0`, `Interpreter` threw `ReturnSignal` C++ exceptions on every `return` statement (invoking the C++ DWARF stack unwinder `21,891` times during `fib(20)`), formatted a `call_stack_` string on every call, and copied the global `unordered_map` on every call.
   - **Optimization**: Replaced `throw ReturnSignal` with a structured `has_return_` / `return_val_` flag in `Interpreter`, deferred `call_stack_` string formatting to `debug_hook_` sessions (`rhea debug`), and kept `global_scope_` / `globals` as a read-only fallback instead of copying it into every activation record. This reduced `W2_recursion_fib20` in `rhea run` from `281.87 ms` to `19.01 ms` (**`14.83x` speedup**).

---

## 4. Reproducing the Benchmarks

```bash
python3 benchmarks/run_benchmarks.py
```
