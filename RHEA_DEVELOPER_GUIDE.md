# Rhea v0.13.1 — Developer Guide

Welcome to **Rhea v0.13.1**, a statically typed, AI-native programming language with three execution backends (`rhea run`, `rhea vm`, and `rhea compile`), a deterministic code formatter (`rhea fmt`), a static linter (`rhea lint`), an interactive CLI debugger (`rhea debug`), a JSON-RPC Language Server (`rhea lsp`), a lockfile-verified package manager (`rhea pkg`), a C++20 native tensor/autodiff runtime (`librhea_native`), and a complete multi-platform minimalist geometric `R` icon and editor integration suite.

---

## 1. Quick Start (Verified Commands)

Every command below has been executed and verified against the `v0.13.1` repository:

```bash
# 1. Build the C++20 native runtime library, compiler binary, and C++ kernel test binary
make all

# 2. Check the compiler version and ABI version
./bin/rhea version
# Output: Rhea Compiler & Runtime v0.13.1 (C ABI v2)

# 3. Run a Rhea program in the tree-walk interpreter
./bin/rhea run examples/01_fibonacci_and_primes.rhea

# 4. Run the same program in the stack bytecode VM (with streaming range & list loop fast paths)
./bin/rhea vm examples/01_fibonacci_and_primes.rhea

# 5. Compile a Rhea program to a standalone native C++20 binary and execute it
./bin/rhea compile examples/01_fibonacci_and_primes.rhea -o build/fib_native
./build/fib_native

# 6. Run the full 165-test end-to-end verification suite
./bin/rhea test

# 7. Run the 32-suite (96-execution) cross-backend differential conformance harness
python3 tests/differential_harness.py

# 8. Regenerate and validate the multi-platform Rhea icon & desktop integration suite
python3 tools/generate_icons.py
python3 tools/validate_icons.py
```

---

## 2. Creating a New Project (`rhea new`)

Scaffold a ready-to-run Rhea project in any directory (including paths with spaces):

```bash
./bin/rhea new my_project
cd my_project
../bin/rhea run src/main.rhea
../bin/rhea test
```

`rhea new` generates:
- `rhea.toml` — Project manifest with package metadata and dependencies
- `src/main.rhea` — Entry-point program
- `tests/basic_test.rhea` — Starter test file
- `.gitignore` — Ignores `build/`, `bin/`, and `*.ckpt`

---

## 3. Core Language Guide

### 3.1 Bindings & Mutability
Bindings are **immutable by default**. Use `mut` when a variable needs to be reassigned or mutated in place (`push`, `pop`, `remove_key`, index assignment):

```rhea
fn main() {
    name = "Rhea"          // Immutable String (inferred)
    let limit: Int = 100   // Explicit type annotation
    const MAX_RETRIES = 3  // Compile-time / module-level constant

    mut score = 10         // Mutable Int
    score += 5
    print("{name} score: {score} (max retries: {MAX_RETRIES})")
}
```

### 3.2 Control Flow, Ranges & Duration Literals
Inclusive (`a..=b`) and exclusive (`a..b`) ranges stream in $O(1)$ memory inside `for` loops across both `rhea run` and `rhea vm`:

```rhea
fn main() {
    mut sum = 0
    for i in 1..=10 {
        sum += i
    }
    print("Sum 1..=10 = {sum}")
    sleep(10ms) // First-class duration literals: 500ms, 1s, 2.5s
}
```

### 3.3 Functions, Closures & Pipelines
Functions support explicit `return` or an implicit final expression (including branching `if / else` expressions). Closures capture lexical variables by value snapshot at creation time and work identically across `rhea run`, `rhea vm`, and `rhea compile`:

```rhea
fn apply_twice(x: Int, f: Fn) -> Int {
    f(f(x))
}

fn main() {
    offset = 7
    add_offset = |v: Int| v + offset
    result = 10 |> add_offset
    print("piped={result}, twice={apply_twice(10, add_offset)}")
}
```

### 3.4 Structs, Generics & Algebraic Enums with `match`
Structs, generic functions (`fn id<T>`), generic structs (`struct Box<T>`), and algebraic `enum` declarations with `match` work identically across **all three backends** (`rhea run`, `rhea vm`, and `rhea compile`):

```rhea
enum Outcome {
    Ok(Int),
    Err(String),
    Empty
}

fn describe(o: Outcome) -> Int {
    match o {
        Outcome::Ok(v) => v * 2,
        Outcome::Err(msg) => -1,
        Outcome::Empty => 0
    }
}

fn main() {
    print("{describe(Outcome::Ok(21))}, {describe(Outcome::Err(\"bad\"))}, {describe(Outcome::Empty)}")
}
```

---

## 4. Standard Library Modules (`std::*`)

All standard library modules below are type-checked by `rhea check` and supported across **all three backends** (`rhea run`, `rhea vm`, and `rhea compile`):

| Module | Functions |
|---|---|
| `std::math` | `abs(x)`, `min(a, b)`, `max(a, b)`, `clamp(x, lo, hi)`, `pow(a, b)`, `sqrt(x)` |
| `std::string` | `len(s)`, `char_count(s)`, `trim(s)`, `upper(s)`, `lower(s)`, `contains(s, sub)`, `starts_with(s, pre)`, `ends_with(s, suf)`, `split(s, delim)`, `join(parts, sep)`, `replace(s, old, new)` |
| `std::io` | `print(v)`, `println(v)`, `print_inline(v)`, `eprintln(v)`, `input(prompt?)`, `read_line()`, `is_eof()`, `try_parse_int(s)`, `try_parse_float(s)`, `parse_int(s)`, `parse_float(s)` |
| `std::fs` | `exists(p)`, `read_text(p)`, `read_lines(p)`, `write_text(p, s)`, `append_text(p, s)`, `remove_file(p)`, `mkdir_p(p)` |
| `std::path` | `join(a, b)`, `basename(p)`, `dirname(p)`, `extension(p)` |
| `std::env` | `get(key, default?)`, `set(key, val)`, `has(key)` |
| `std::time` | `now_ms()`, `sleep(ms)` |
| `std::json` | `parse(s)` (64-level recursion guard), `stringify(v)` |
| `std::log` | `info(msg)`, `warn(msg)`, `error(msg)` |
| `std::test` | `assert(cond, msg?)`, `assert_eq(actual, expected, msg?)` |

---

## 5. Brand Identity, Icon Suite & Desktop Integration

See [**`RHEA_ICON_GUIDE.md`**](RHEA_ICON_GUIDE.md) for full details on the minimalist geometric **`R` monogram** icon system:
- **Primary Dark Icon**: Dark slate-navy `#0F172A` tile + solid teal `#14B8A6` `R` monogram.
- **Light-Background Variant**: Near-white `#F8FAFC` tile + deep teal `#0F766E` `R` monogram.
- **Assets**: Editable SVGs, `16×16`..`1024×1024` dark & light RGBA PNGs, native macOS `assets/icons/rhea.icns`, native Windows `assets/icons/rhea.ico`, and `assets/icons/rhea-social-preview.png`.
- **Integrations**: VS Code (`tools/vscode-rhea/`), macOS (`tools/desktop/macos/`), Windows (`tools/desktop/windows/`), and Linux (`tools/desktop/linux/`).
