# Rhea Language Specification (`v0.13.0`)

## 1. Variable Declarations, Mutability, Typo Protection & Shadowing
- **Concise Immutable Binding (`name = expr`)**: When `name` is not currently bound in any enclosing scope of the function, `name = expr` declares a new immutable local variable (`is_mutable = false`).
- **Typo Protection on Assignment (`E0004`)**: If an undeclared identifier in `ident = expr` is within Damerau-Levenshtein edit distance 1–2 of an existing `mut` variable in scope (excluding digit-only differences like `p1` vs `p2`, e.g., `mut count = 0; conut = 1`), the compiler rejects the statement with `E0004` and suggests the existing mutable variable (`Did you mean 'count = ...'?`).
- **Explicit Bindings**: `let x = 10`, `let score: Float = 95.5`, `mut count = 0`, `let mut count = 0`, and `const MAX = 100`.
- **Reassignment & Nested Lvalue Mutation Rules (`E0005`, `E0003`, `E1003`)**:
  - Reassigning (`=`, `+=`, `-=`, `*=`, `/=`) or mutating any immutable binding fails at compile time with `E0005`.
  - When the root binding is declared with `mut`, arbitrarily nested list elements, map entries, and struct fields (`matrix[i][j] = val`, `matrix[i][j] += val`, `obj.items[i] = val`, `obj.inner.field += val`) can be mutated across `rhea run`, `rhea vm`, and `rhea compile`.
  - Each index expression along a nested lvalue chain is evaluated **exactly once in left-to-right order** before the right-hand side expression and before mutating the leaf target. Out-of-bounds list indices or missing intermediate map keys trap at runtime with `E1003`.
- **Shadowing & Lexical Scope**:
  - Variables declared with `let` or `mut` inside an inner block (`{ ... }`, `if`, `while`, `for`) shadow outer bindings of the same name until the inner block exits.
  - Same-scope re-declaration with explicit `let x = ...` creates a new shadowed binding without mutating earlier bindings or closures that captured the previous binding.

## 2. Numeric Semantics, Overflow Traps & Duration Literals
- **64-Bit Signed `Int` (`[-9,223,372,036,854,775,808 .. 9,223,372,036,854,775,807]`)**:
  - Both `-9223372036854775808` (`INT64_MIN`) and `9223372036854775807` (`INT64_MAX`) are valid integer literals.
  - Signed 64-bit integer addition (`+`), subtraction (`-`), multiplication (`*`), division (`INT64_MIN / -1`), and unary negation (`-INT64_MIN`) are checked at runtime and trap with `E1002` on overflow across `rhea run`, `rhea vm`, and `rhea compile`.
  - Division (`/`) and modulo (`%`) by zero trap with `E1001` across all three backends. `INT64_MIN % -1` evaluates safely to `0` across all three backends without undefined behavior.
- **64-Bit IEEE-754 `Float` & Duration Literals**:
  - Floating-point literals overflowing IEEE-754 `double` (e.g., `1.0e500`) and duration literals (`ms`, `s`) overflowing 64-bit integer milliseconds are rejected at compile time with `E1002`.

## 3. Type System, Functions, Enums, Pattern Matching & Implicit Returns
- **Static Type Checking**: Enforces type compatibility across arithmetic, comparisons, logical operators (`&&`, `||`, `!`), homogeneous `List<T>` literals, `Map<K, V>` literals, `Struct` field initializers/accesses, `Enum` variant constructors, and `std::*` standard library calls.
- **Algebraic Enums & `match` Expressions**:
  - Enums may declare unit variants or typed payload variants (`enum ResultCode { Ok(Int), Err(String) }`).
  - `match` expressions match against enum variants (`ResultCode::Ok(v) => v * 2`), literals, variable bindings, or `_` wildcards across `rhea run`, `rhea vm`, and `rhea compile`.
- **Function Return Semantics & Definite Return Analysis (`E0007`)**:
  - Functions omitting `-> Type` default to `Unit` (`fn main() { ... }`).
  - Functions declaring `fn main() -> Int` propagate their returned integer as the process exit status across `rhea run`, `rhea vm`, and `rhea compile`.
  - Every non-`Unit` function is statically checked to ensure all control-flow paths return a value (`E0007`).

## 4. First-Class Lexical Closures (`|params| body`)
- **Capture Model**:
  - Closures capture enclosing local variables via a **deterministic, immutable lexical value snapshot** at the moment the closure is constructed.
  - Attempting to reassign or mutate a captured outer variable inside a closure fails at compile time with `E0005`.
  - Closures may freely declare and mutate their own local `mut` bindings and nest closures across all three backends.

## 5. Generic Functions & Generic Structs
- Functions (`fn f<T, U>(...)`) and structs (`struct Pair<K, V> { ... }`) unify generic type parameters against concrete argument/field types at each instantiation site and validate body operators against bound types (`E0003`).

## 6. Modules, Imports & Visibility (`module`, `import`, `pub fn`, `priv fn`)
- Validates `std::*` imports against the official standard library registry (`E0009`).
- Resolves local modules relative to the importing file while rejecting `..` path traversal (`E0009`), circular imports (`E0009`), private function calls across module boundaries (`E0009`), and cross-module duplicate public symbol collisions (`E0002`).

## 7. Ranges & Iteration Semantics
- **Inclusive (`start..=end`) & Exclusive (`start..end`) Ranges**:
  - Streamed in $O(1)$ memory when iterated in `for i in start..=end` / `for i in start..end` across `rhea run`, `rhea vm`, and `rhea compile`.
  - Empty or descending bounds (`5..5`, `5..=3`) execute zero iterations, and `INT64_MAX..=INT64_MAX` executes 1 iteration without signed overflow.
