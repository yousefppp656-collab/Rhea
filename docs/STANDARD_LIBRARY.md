# Rhea Standard Library Reference (`v0.9.3`)

## Top-Level Built-ins (No Import Required)
- `print(...)`, `println(...)`, `print_inline(...)`: Write formatted output to `stdout` and flush.
- `input(prompt?) -> String`: Print optional prompt without newline, flush `stdout`, and read one line from `stdin`.
- `is_eof() -> Bool`: Return `true` when `stdin` has reached EOF.
- `try_parse_int(s: String) -> Map`: Safely parse a 64-bit integer, returning `{"ok": Bool, "value": Int, "error": String}`.
- `try_parse_float(s: String) -> Map`: Safely parse a double-precision float, returning `{"ok": Bool, "value": Float, "error": String}`.
- `parse_int(s: String) -> Int`, `parse_float(s: String) -> Float`, `input_int(prompt?) -> Int`, `input_float(prompt?) -> Float`.
- `sleep(duration_ms: Int)`: Pause the current thread for `duration_ms` milliseconds (`1s` = `1000`, `500ms` = `500`).
- `now_ms() -> Int`: Return monotonic clock time in milliseconds.
- `trim(s: String) -> String`, `lower(s: String) -> String`, `upper(s: String) -> String`.
- `len(collection) -> Int`, `str(value) -> String`, `int(value) -> Int`, `float(value) -> Float`, `push(list, item)`.
- `assert(cond, msg?)`, `assert_eq(left, right)`.

## Standard Modules
- `std::io`: `read_line()`, `input(prompt?)`, `is_eof()`, `try_parse_int(s)`, `try_parse_float(s)`, `print_inline(...)`
- `std::time`: `sleep_ms(ms)`, `monotonic_ms()`
- `std::string`: `trim(s)`, `to_lower(s)`, `to_upper(s)`, `split(s, delim)`, `contains(s, sub)`
- `std::math`: `abs(x)`, `sqrt(x)`, `sin(x)`, `cos(x)`, `exp(x)`, `log(x)`, `clamp(x, lo, hi)`
- `std::ai`: `tensor(shape, data)`, `matmul(a, b)`, `relu(t)`, `sigmoid(t)`, `tanh(t)`, `softmax(t)`, `sum(t)`, `mean(t)`, `l2_norm(t)`, `mse_loss(pred, target)`, `train_xor_demo(epochs)`
- `std::fs`: `read_text(path)`, `write_text(path, content)`, `exists(path)`
