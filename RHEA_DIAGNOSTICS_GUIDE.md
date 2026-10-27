# Rhea v0.12.0 — Compiler & Runtime Diagnostics Guide

This guide documents Rhea's diagnostic format, every compile-time, runtime, backend, and CLI error code (`E0001`–`E3002`), the Levenshtein suggestion engine, and practical workflows for diagnosing and fixing errors in Rhea programs.

---

## 1. Anatomy of a Rhea Diagnostic

Every compile-time and runtime error emitted by the `rhea` compiler, bytecode VM, or native C++20 runtime follows a uniform, machine- and human-readable format on `stderr`:

```text
error[E0005]: cannot reassign immutable variable 'count'
  --> src/main.rhea:4:5
   |
 4 |     count = count + 1
   |     ^^^^^
   = why: Variables created with 'name = value' or 'let name = value' are immutable by default to prevent accidental state changes.
   = help: Declare 'count' as mutable using 'mut count = ...' or 'let mut count = ...'.
   = example: mut count = 0
              count = count + 1
```

Each diagnostic consists of up to six structured elements:
1. **Severity and Stable Error Code**: `error[E0005]: <message>` — uniquely identifies the diagnostic class and matches the `code` field published by `rhea lsp`.
2. **Source Location**: `--> <filepath>:<line>:<col>` using 1-based line and column numbers (`rhea lsp` automatically converts these to 0-based UTF-16 code units).
3. **Source Excerpt & Caret Underline**: Displays the exact line of source code with `^` carets spanning the offending token or expression.
4. **`= why:` Explanation**: Explains the language rule or safety invariant that was violated.
5. **`= help:` Actionable Fix**: Provides a concrete fix—including Levenshtein edit-distance identifier suggestions when a variable, function, struct, field, module, or CLI command is misspelled.
6. **`= example:` Valid Code Pattern**: Shows a minimal valid Rhea snippet demonstrating the idiomatic fix where applicable.

---

## 2. Levenshtein Edit-Distance Suggestion Engine

When an identifier or command is not found, Rhea computes the Damerau-Levenshtein edit distance (insertions, deletions, substitutions, and adjacent transpositions up to edit distance `2`) against all valid candidates in scope:

| Context | Candidate Pool Searched | Diagnostic Code | Example Suggestion |
| :--- | :--- | :--- | :--- |
| Local or global variable read/write | All visible bindings in lexical scope chain | `E0004` | `Did you mean 'count'?` |
| Assignment to undeclared variable (`conut = 1`) | Visible mutable bindings (`edit_distance <= 2`, excluding digit-only differences like `p1`/`p2`) | `E0004` | `'conut' is not declared, and is 1 character(s) away from mutable variable 'count'` |
| Function call (`compute_totl(5)`) | User-defined functions, imported module functions, and built-in functions | `E0004` | `Did you mean 'compute_total(...)'?` |
| Struct initialization (`Point2d { ... }`) | All declared `struct` types in scope | `E0004` | `Did you mean 'struct Point2D'?` |
| Struct field access/init (`cfg.timeout_m`) | Declared fields of the target `struct` | `E0004` | `Did you mean field 'timeout_ms' on 'Config'?` |
| Standard library import (`import std::strng`) | Official `std::*` module registry (`std::io`, `std::math`, `std::string`, `std::collections`, `std::time`, `std::fs`, `std::json`, `std::path`, `std::env`, `std::log`, `std::ai`) | `E0009` | `Did you mean 'import std::string'?` |
| CLI subcommand (`rhea rn app.rhea`) | All valid CLI subcommands (`run`, `vm`, `check`, `compile`, `build`, `fmt`, `lint`, `lsp`, `debug`, `test`, `new`, `init`, `pkg`, `version`, `help`) | `E3002` | `Did you mean 'rhea run'?` |

---

## 3. Complete Error Code Catalog

### 3.1 Compile-Time Front-End & Static Analysis Errors (`E0001`–`E0009`)

#### `E0001` — Lexical Error
- **Meaning**: The lexer encountered an invalid character, unterminated string literal, unterminated block comment (`/* ...`), malformed scientific notation exponent (`1.0e+`), or unsupported numeric suffix (`10px`).
- **Common Trigger**:
  ```rhea
  fn main() {
      delay = 500us
  }
  ```
- **How to Fix**: Close quotes/comments properly and use supported duration suffixes (`s` for seconds or `ms` for milliseconds, e.g., `500ms`, `1.5s`).

#### `E0002` — Syntax, Parse Depth, or Duplicate Declaration Error
- **Meaning**: The parser encountered an unexpected token, missing delimiter, duplicate function/closure parameter name, duplicate struct field declaration, duplicate field in a struct initializer, cross-module symbol collision, or exceeded the maximum parse nesting depth (`256`).
- **Common Trigger**:
  ```rhea
  struct Point { x: Int, x: Int }
  ```
- **How to Fix**: Ensure every parameter and struct field has a unique name and check matching `{ ... }`, `( ... )`, and `[ ... ]` delimiters.

#### `E0003` — Static Type Mismatch
- **Meaning**: An operator, variable annotation, collection literal, index expression, assignment target, or function/generic argument received an incompatible type.
- **Common Trigger**:
  ```rhea
  fn main() {
      mut matrix = [[1, 2], [3, 4]]
      matrix[0][1] = "wrong_type"
  }
  ```
- **How to Fix**: Ensure operands match the expected static type (`Int`, `Float`, `Bool`, `String`, `List<T>`, `Map<String, V>`, or `Struct`), or explicitly convert values using `str(x)`, `int(x)`, or `float(x)`.

#### `E0004` — Undefined Variable, Function, Struct, or Field
- **Meaning**: A referenced variable, function, struct type, or struct field does not exist in the current scope. Also triggered by the typo guard when an implicit declaration (`conut = 1`) is within edit distance 1–2 of an existing mutable variable (`mut count = 0`).
- **Common Trigger**:
  ```rhea
  struct Config { timeout_ms: Int }
  fn main() {
      c = Config { timeout_ms: 100 }
      print(c.timeout_m)
  }
  ```
- **How to Fix**: Check the `= help: Did you mean ...` suggestion or declare the binding before use.

#### `E0005` — Immutability or Closure Capture Mutation Violation
- **Meaning**: The program attempted to reassign or mutate (`=`, `+=`, `-=`, `*=`, `/=`, `obj.field = ...`, `matrix[i][j] = ...`, `push`, `pop`, `remove_key`) a binding declared without `mut`, a `const` constant, or an outer variable captured inside a closure.
- **Common Trigger**:
  ```rhea
  fn main() {
      mut total = 0
      add = |x: Int| -> Int {
          total += x
          return total
      }
  }
  ```
- **How to Fix**:
  - For regular variables, declare the root binding with `mut` (`mut matrix = [[1, 2], [3, 4]]`).
  - For closures, remember that Rhea closures capture outer variables by **immutable lexical value snapshot**. Return the updated value from the closure instead (`total = add(total, x)`), or declare local mutable state (`mut local_acc = ...`) inside the closure body.

#### `E0006` — Function or Built-in Arity Mismatch
- **Meaning**: A function, closure, or built-in call passed fewer or more arguments than declared in its signature.
- **Common Trigger**:
  ```rhea
  fn add(a: Int, b: Int) -> Int { return a + b }
  fn main() { print(add(10)) }
  ```
- **How to Fix**: Pass the exact number of arguments required by the function signature.

#### `E0007` — Missing Return on Control-Flow Path
- **Meaning**: A function with an explicit non-`Unit` return type (`-> Int`, `-> String`, etc.) can reach the end of its body without returning a value.
- **Common Trigger**:
  ```rhea
  fn classify(x: Int) -> String {
      if x > 0 { return "positive" }
  }
  ```
- **How to Fix**: Add an `else` branch or a final fallback `return` statement so every control-flow path returns a value of the declared type.

#### `E0008` — Loop Control Statement Outside Loop
- **Meaning**: `break` or `continue` was used outside a `for`, `while`, or `parallel for` loop (or across a function/closure boundary).
- **How to Fix**: Place `break` and `continue` directly inside the body of an enclosing loop.

#### `E0009` — Module Resolution, Visibility, or Circular Import Error
- **Meaning**: An `import` statement referenced a non-existent local module or unknown `std::*` module, attempted parent-directory traversal (`..`), formed a circular import chain (`A -> B -> A`), or called a `priv fn` helper from outside its defining module.
- **How to Fix**: Keep local module files inside the project directory, break circular dependencies, mark shared functions `pub fn` (or default `fn`), and verify `std::*` module spelling.

---

### 3.2 Runtime Execution Errors (`E1001`–`E1008`)

#### `E1001` — Division or Modulo by Zero
- **Meaning**: Integer or floating-point division (`/`, `/=`) or integer remainder (`%`) was evaluated with a zero divisor (`0` or `0.0`). Enforced identically in `rhea run`, `rhea vm`, and `rhea compile`.
- **How to Fix**: Check that the divisor is non-zero (`if denom != 0 { ... }`) before dividing.

#### `E1002` — Checked 64-Bit Signed Integer Overflow
- **Meaning**: Signed 64-bit integer addition (`+`), subtraction (`-`), multiplication (`*`), division (`INT64_MIN / -1`), unary negation (`-INT64_MIN`), or integer literal parsing overflowed the range `[-9,223,372,036,854,775,808 .. 9,223,372,036,854,775,807]`. Enforced identically across `rhea run`, `rhea vm`, and `rhea compile`.
- **How to Fix**: Guard arithmetic bounds or use `Float` (`float(x)`) for values exceeding 64-bit integer limits.

#### `E1003` — Index Out of Bounds or Missing Map Key
- **Meaning**: A list read/write index was outside `0 .. len(list) - 1`, `pop()` was called on an empty list, or a map key lookup/nested mutation referenced a key that does not exist.
- **How to Fix**: Check `i >= 0 && i < len(xs)` before indexing a list, and check `has_key(m, key)` before reading or mutating a nested entry under `m[key]`.

#### `E1004` — Recursion Depth, VM Step, or Range Allocation Limit Exceeded
- **Meaning**: Call stack depth exceeded `MAX_RECURSION_DEPTH` (`256`), VM execution exceeded `MAX_VM_STEPS` (`50,000,000`), or a materialized range expression (`a..=b`) exceeded `MAX_RANGE_ELEMENTS` (`10,000,000`).
- **How to Fix**: Ensure recursive functions have a reachable base case and iterate over large ranges directly in `for i in start..=end` (which streams O(1) state) rather than materializing a massive list.

#### `E1005` — Assertion Failure
- **Meaning**: `assert(cond)` or `assert_eq(actual, expected)` failed at runtime.
- **How to Fix**: Inspect the printed actual vs. expected values in the diagnostic message and fix the failing invariant.

#### `E1006` — Parse, Format, or File I/O Runtime Error
- **Meaning**: `int(s)` / `parse_int(s)` / `float(s)` received a malformed numeric string, `std::json.parse(s)` encountered invalid JSON syntax, or `std::fs.*` could not read/write/remove the target file path.
- **How to Fix**: For untrusted user input, use `try_parse_int(s)` (which returns `{"ok": Bool, "value": Int, "error": String}` without throwing) or check `std::fs.exists(path)` before reading.

#### `E1008` — Invalid Sleep Duration
- **Meaning**: `sleep(ms)` was called with a negative duration (`< 0`) or a duration exceeding `3,600,000` ms (1 hour).
- **How to Fix**: Pass a non-negative duration such as `sleep(50ms)` or `sleep(1s)`.

---

### 3.3 Backend, Bytecode & Native Codegen Errors (`E2001`–`E2003`)

#### `E2001` — Bytecode Verification Error
- **Meaning**: The pre-execution bytecode verifier (`BytecodeChunk::verify`) detected an out-of-range constant pool index, invalid jump target, or unresolved function reference.

#### `E2002` — Unsupported Backend Capability
- **Meaning**: `validate_backend_capabilities()` detected a language construct or dynamic runtime feature that is not supported by the requested execution backend before compilation/execution:
  - `try / catch`, `std::ai.*` / `tensor()`, or `pipeline` in `rhea vm` or `rhea compile`.
  - Dynamic `std::json.*` (`std::json.parse` / `std::json.stringify`) in `rhea compile` (supported in both `rhea run` and `rhea vm`).
- **How to Fix**: Follow the `= help:` instruction in the diagnostic (for example, run with `rhea run <file.rhea>` or `rhea vm <file.rhea>`). See [`RHEA_BACKEND_CAPABILITIES.md`](RHEA_BACKEND_CAPABILITIES.md) for the complete backend matrix.

#### `E2003` — Native C++20 Compiler Invocation Error
- **Meaning**: `rhea compile` or `rhea build` generated C++20 code, but the system `c++` / `clang++` compiler failed to compile or link the binary.

---

### 3.4 CLI & Project Errors (`E3001`–`E3002`)

#### `E3001` — Source File or Project Not Found
- **Meaning**: The specified `.rhea` source file, directory, or `rhea.toml` manifest could not be opened.

#### `E3002` — Unknown CLI Subcommand or Missing Flag Argument
- **Meaning**: An unknown subcommand was passed to `rhea` (e.g., `rhea rn app.rhea`) or a required flag argument (such as `rhea compile app.rhea -o`) was omitted.
