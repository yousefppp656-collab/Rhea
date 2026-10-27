# Rhea v0.11.0 — Language Specification & Cross-Backend Conformance Matrix (`RHEA_LANGUAGE_CONFORMANCE.md`)

## 1. Precise Language Specification (`v0.11.0`)

### 1.1 Variable Declaration, Reassignment, Mutability & Shadowing
- **Concise Immutable Declaration (`name = expr`)**: If `name` is not bound in any active lexical scope of the current function, `name = expr` declares a new immutable local binding (`is_mutable = false`).
- **Typo Protection Guard (`E0004`)**: If `name` in `name = expr` is undeclared and has Damerau-Levenshtein edit distance `1` from an existing `mut` variable in scope (excluding purely numeric suffix differences like `v1` vs `v2`), compilation fails with `E0004` and suggests the existing mutable variable. Explicit `let name = expr` overrides the guard when a new similarly named variable is intended.
- **Explicit Declarations**:
  - Immutable: `let x = 10`, `let x: Int = 10`, `x: Int = 10`, `const MAX = 100`.
  - Mutable: `mut x = 10`, `let mut x = 10`, `let mut m: Map<String, Int> = {}`, `let mut xs: List<String> = []`.
- **Reassignment & In-Place Mutation (`E0005`)**:
  - Reassigning (`=`, `+=`, `-=`, `*=`, `/=`) an immutable variable fails at compile time with `E0005`.
  - Mutating an element, key, or field of an immutable `List`, `Map`, or `Struct` (`xs[i] = v`, `m[k] = v`, `s.field = v`, `push(xs, v)`, `pop(xs)`, `remove_key(m, k)`) fails at compile time with `E0005`.
- **Lexical Scoping & Shadowing**:
  - Standalone `{ ... }` blocks, `if`/`else` branches, `while` bodies, `for` loops, and `try`/`catch` blocks introduce nested lexical scopes.
  - Declarations with `let` or `mut` inside an inner scope shadow outer bindings until the inner scope exits.
  - The `TypeChecker` assigns each lexical binding a unique resolved identifier (`x`, `x__s2`, `x__s3`), guaranteeing identical scoping across `rhea run`, `rhea vm`, and `rhea compile`.
  - Top-level `const` bindings are visible inside all functions and closures across all three backends.

### 1.2 Primitive Types, Collections, Structs & Type Inference
- **Primitive Types**: `Int` (signed 64-bit two's-complement integer `int64_t`), `Float` (IEEE-754 64-bit `double`), `Bool` (`true`, `false`), `String` (UTF-8 byte sequence), and `Unit` (`()`).
- **Homogeneous Lists (`List<T>`)**:
  - Non-empty literals `[e1, e2, ...]` infer `List<T>` from `e1` and require all subsequent elements to match `T` (`E0003` on heterogeneous elements).
  - Empty list literals `[]` infer `List<Any>` or adopt an explicit annotation `let mut xs: List<String> = []`.
  - Indexing `xs[i]` requires `i: Int` (`E0003` at compile time) and `0 <= i < len(xs)` (`E1003` at runtime).
- **Homogeneous Maps (`Map<K, V>`)**:
  - Keys `K` must be `String` or `Int` (`E0003` on other key types). All keys must share type `K` and all values must share type `V` (`E0003` on heterogeneous entries).
  - Empty map literals `{}` or `{:}` adopt their explicit type annotation (`let mut m: Map<String, Int> = {}`) or default to `Map<String, Int>`.
  - Reading `m[k]` raises runtime error `E1003` if `k` is absent; writing `m[k] = v` inserts or updates key `k` on a `mut` map.
- **Structs (`struct Name<T1, ...>`)**:
  - Field names in a `struct` declaration must be unique (`E0002`).
  - Struct initializers `Name { f1: v1, f2: v2 }` may specify fields in any order, must provide every declared field exactly once (`E0002` on duplicate field, `E0003` on missing field or type mismatch, `E0004` on unknown field), and support `==`, `!=`, `str(s)`, `print(s)`, and `s.field` access/mutation across all three backends.

### 1.3 Function Calls, Parameters, Return Values & Recursion
- **Parameters & Arity**: Parameter names within a function or closure must be unique (`E0002`). Calls must pass the exact parameter count (`E0006`) and compatible argument types (`E0003`).
- **Return Semantics (`E0007`)**:
  - Functions omitting `-> Type` default to `Unit`.
  - Functions declaring a non-`Unit` return type must return a compatible value on all control-flow paths (`all_paths_return`, `E0007`), either via explicit `return expr` or via an implicit final expression (including final `if / else` statements where both branches return).
- **Process Exit Status**: If `fn main() -> Int` returns an integer `k`, the process exits with status `k` across `rhea run`, `rhea vm`, and `rhea compile`. If `fn main()` has return type `Unit`, the process exits with `0`.
- **Recursion**: Direct and mutual recursion are supported across all three backends up to `MAX_RECURSION_DEPTH = 256` (`E1004` in `run` and `vm`).

### 1.4 Generics, Type Inference, Constraints & Specialization
- **Generic Functions & Structs**: Declared with `<T>` or `<K, V>` (e.g., `fn lookup_or<K, V>(m: Map<K, V>, k: K, def: V) -> V`, `struct Entry<K, V> { key: K, val: V }`).
- **Unification**: `unify_type_pattern` recursively splits top-level comma-separated type arguments inside `List<...>`, `Map<..., ...>`, and generic structs, binding type parameters and rejecting conflicting concrete types for the same type parameter with `E0003`.
- **Body Operator Validation**: At each generic call site, `validate_generic_stmt_ops` recursively traverses the generic function's AST and verifies that every unary/binary operator (`+`, `-`, `*`, `/`, `%`, `<`, `<=`, `>`, `>=`) applied to generic parameters is valid for the bound concrete types (`E0003`).

### 1.5 Closures, Captures, Mutation & Lifetime
- **Syntax**: `|x: Int, y: Int| x + y`, `|| 42`, or `|x: Int| -> Int { return x * 2 }`.
- **Lexical Value Snapshot**: Closures capture enclosing local variables by value at the point of closure construction. Subsequent mutations to an outer `mut` variable do not affect already-constructed closures. Closures can be stored in variables and passed as `Fn` parameters to higher-order functions across all three backends.

### 1.6 Inclusive & Exclusive Ranges, Integer Boundaries & Loops
- **Range Operators**: `start..end` (exclusive) and `start..=end` (inclusive) require `Int` bounds (`E0003`).
- **Boundary & Empty Semantics**:
  - If `end <= start` for `..` or `end < start` for `..=`, the range evaluates to `[]` (zero iterations).
  - Negative ranges (`-3..=2` -> `[-3, -2, -1, 0, 1, 2]`) and zero ranges (`0..=0` -> `[0]`, `0..0` -> `[]`) are supported across all backends.
  - `INT64_MAX..=INT64_MAX` executes exactly 1 iteration without signed overflow.
  - Ranges exceeding `MAX_RANGE_ELEMENTS = 10,000,000` trap with `E1004` across all three backends.
- **Loops (`while`, `for`)**: `break` and `continue` affect only the innermost enclosing loop and are rejected outside loops at compile time with `E0008`.

### 1.7 Modules, Visibility & Filesystem Boundaries
- **Imports**: `import std::<mod>` validates against the built-in standard library catalog (`E0009`). Local imports (`import math_utils` or `import modules::geometry`) resolve relative `.rhea` files.
- **Security & Visibility (`E0009`, `E0002`)**:
  - Parent directory traversal (`..`) and absolute paths are rejected with `E0009`.
  - Circular module imports (`A -> B -> A`) are rejected with `E0009` and full cycle trace.
  - `priv fn` functions are callable only within their defining module; external calls fail with `E0009`.
  - Duplicate public function names across imported modules fail with `E0002`.

### 1.8 Compile-Time vs. Runtime Error Catalog
- **Compile-Time Errors (Exit Code `1`)**:
  - `E0001`: Lexical error (invalid character, unterminated string/comment, invalid numeric suffix).
  - `E0002`: Syntax / declaration error (unexpected token, duplicate function/struct/field/parameter, nesting depth `> 256`).
  - `E0003`: Static type mismatch (incompatible types, invalid operator operand types, generic unification conflict).
  - `E0004`: Undefined identifier/field/struct or Damerau-Levenshtein distance-1 mutable variable typo guard.
  - `E0005`: Mutation of an immutable binding, list, map, or struct.
  - `E0006`: Function or closure argument count (arity) mismatch.
  - `E0007`: Missing return statement on control-flow path in non-`Unit` function.
  - `E0008`: `break` or `continue` used outside a loop.
  - `E0009`: Module resolution error (missing module, circular dependency, `..` traversal, `priv fn` access).
- **Runtime Errors (Exit Code `1`)**:
  - `E1001`: Division or modulo by zero (`Int` or `Float`).
  - `E1002`: Signed 64-bit integer overflow (`+`, `-`, `*`, `INT64_MIN / -1`, `-INT64_MIN`).
  - `E1003`: List index out of bounds, `pop()` on empty list, or missing key lookup in `Map<K, V>`.
  - `E1004`: Recursion depth (`> 256`), VM step limit (`> 50,000,000`), or range allocation limit (`> 10,000,000`) exceeded.
  - `E1005`: Assertion failure (`assert` / `assert_eq`).
  - `E1006`: Malformed number parsing (`parse_int`/`parse_float`), malformed JSON (`std::json.parse`), or missing file I/O (`std::fs.read_text`).
  - `E1007`: Native C ABI tensor/kernel error.
  - `E1008`: Invalid negative or excessive `sleep()` duration.

## 2. Cross-Backend Semantic Conformance Matrix (`v0.11.0`)

| Semantic Category | `rhea run` (Interpreter) | `rhea vm` (Bytecode VM) | `rhea compile` (Native C++20) | Verification Suite(s) |
|---|---|---|---|---|
| **1. Arithmetic, Comparisons, Precedence & Short-Circuiting (`&&`, `||`)** | Conformant | Conformant | Conformant | Diff Suite `14`, Test `111` |
| **2. Signed 64-Bit Overflow (`E1002`) & Division/Modulo by Zero (`E1001`)** | Conformant (`E1001`/`E1002`) | Conformant (`E1001`/`E1002`) | Conformant (`E1001`/`E1002`) | Diff Suites `15`, `16`, Tests `28`, `29`, `96`, `97` |
| **3. Variable Init, Immutability (`E0005`), Typo Guard (`E0004`) & Shadowing** | Conformant | Conformant | Conformant | Diff Suite `01`, Tests `41`, `42`, `56`–`58` |
| **4. Top-Level `const` Visibility in Functions & Closures** | Conformant | Conformant | Conformant | Diff Suite `17`, Test `98` |
| **5. Functions, Recursion, Early Returns & Return-Path Analysis (`E0007`)** | Conformant | Conformant | Conformant | Diff Suite `02`, Tests `54`, `60`–`63` |
| **6. Closures, Lexical Snapshot Captures & Higher-Order `Fn` Parameters** | Conformant | Conformant | Conformant | Diff Suite `03`, Tests `64`, `65`, `110` |
| **7. Single & Multi-Param Generics (`fn<K, V>`, `struct<K, V>`) & Body Op Check** | Conformant | Conformant | Conformant | Diff Suites `04`, `20`, Tests `66`, `67`, `81`, `104` |
| **8. Lists & Maps (`push`, `pop`, `has_key`, `keys`, `values`, `remove_key`, `+=`, `E1003`)** | Conformant | Conformant | Conformant | Diff Suites `08`–`12`, `21`, Tests `76`–`80`, `103`, `108`, `113` |
| **9. Structs (Out-of-Order Init, Field Access/Mutation, `+=`, `==`, `!=`, `print`)** | Conformant | Conformant | Conformant | Diff Suite `19`, Tests `100`, `102` |
| **10. String Ordering (`<, <=, >, >=`), UTF-8 `char_count` & String Helpers** | Conformant | Conformant | Conformant | Diff Suite `18`, Tests `90`, `99`, `109` |
| **11. Ranges (Negative, Zero, `INT64_MAX..=INT64_MAX`, `>10M` `E1004` Guard)** | Conformant | Conformant | Conformant | Diff Suites `05`, `22`, Tests `36`–`40`, `71`, `72`, `108`, `112` |
| **12. Nested Loops (`while`, `for`), `break`, `continue` & `E0008` Guard** | Conformant | Conformant | Conformant | Diff Suite `06`, Test `73` |
| **13. Local Multi-File Modules (`pub fn`, `priv fn`, Cycle & `..` Guards `E0009`)** | Conformant | Conformant | Conformant | Diff Suite `24`, Tests `68`–`70`, `119` |
| **14. Interactive Stdin (`input`, `is_eof`), `eprintln` & `main() -> Int` Exit Code** | Conformant | Conformant | Conformant | Diff Suites `07`, `13`, `23`, Tests `44`–`47`, `93`, `105` |
| **15. Dynamic `std::json.parse` DOM & Native `std::ai.*` Tensor Kernels** | Conformant | Conformant (`std::json`) | Documented Backend Scope (see Section 3) | Tests `05`, `08`, `22`, `23`, `88`, `89` |

## 3. Documented Backend Scope & Unsupported Cases

1. **Dynamic JSON DOM (`std::json.parse`) & `std::ai.*` Tensors**:
   - Fully supported in `rhea run` (and `std::json` in `rhea vm`). `rhea compile` targets static C++20 native types (`int64_t`, `double`, `bool`, `std::string`, `std::vector<T>`, `std::map<K, V>`, and C++20 `struct`/`template` definitions) and does not embed the dynamic `rhea::Value` DOM or link `librhea_native.so` into standalone user binaries by default.
2. **`try { ... } catch err { ... }` Error Recovery**:
   - Supported in `rhea run` (`Interpreter::exec_stmt`). In `rhea vm` and `rhea compile`, runtime traps (`E1001`–`E1008`) terminate the process with exit code `1` and print the diagnostic to `stderr`.
3. **Chained Multi-Dimensional Lvalue Assignment (`xs[i][j] = v`)**:
   - Single-level index/field assignment (`xs[i] = v`, `m[k] = v`, `s.f = v`) is supported across all three backends; nested lvalue assignment requires extracting the inner list/struct into a `mut` variable, mutating it, and assigning it back.
