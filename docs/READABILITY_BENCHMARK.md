# Rhea v0.9.3 Readability Benchmark (10 Programs)

Comparison between `v0.9.2` explicit syntax and `v0.9.3` simplified syntax across all 10 benchmark programs in `examples/readability/`:

| # | Program | `v0.9.2` Lines | `v0.9.3` Lines | Key Simplifications Used |
|---|---|---:|---:|---|
| 1 | `01_hello.rhea` | 5 | 3 | `fn main()` without `-> Int` / `return 0` |
| 2 | `02_variables_and_math.rhea` | 10 | 8 | Concise `price = 25`, `mut total = ...` |
| 3 | `03_user_input_greeting.rhea` | 7 | 4 | Built-in `input(...)` + `trim(...)` + `{name}` interpolation |
| 4 | `04_number_guessing_round.rhea` | 24 | 19 | `try_parse_int(raw)` + concise bindings |
| 5 | `05_countdown_timer.rhea` | 14 | 10 | `for i in 1..=n` + `sleep(10ms)` duration literal |
| 6 | `06_list_filtering_and_sum.rhea` | 16 | 12 | `for n in 1..=10` + `mut evens = []` |
| 7 | `07_word_frequency_counter.rhea` | 15 | 12 | `words = ...`, `mut rhea_count = 0` |
| 8 | `08_file_read_write_lines.rhea` | 12 | 9 | Concise bindings + `len(lines)` |
| 9 | `09_struct_and_methods.rhea` | 16 | 13 | Implicit final-expression return `r.width * r.height` |
| 10 | `10_ai_tensor.rhea` | 9 | 7 | Concise tensor bindings + `fn main()` |
