#!/usr/bin/env python3
"""Differential Cross-Backend Conformance Harness for Rhea v0.13.0 (32 Suites).

Executes identical .rhea programs through:
  1. Tree-Walking Interpreter (`rhea run`)
  2. Stack-Based Bytecode VM (`rhea vm`)
  3. Native C++20 Compiler (`rhea compile` -> native Mach-O / ELF binary)
and verifies:
  - Standard output matches across all 3 backends AND matches an independently specified ground-truth output.
  - Standard error, exit status, and documented runtime error codes match across all 3 backends.
  - Compiler failures are distinguished from runtime program failures.
  - Process timeouts and abnormal signal terminations (SIGSEGV/SIGABRT/SIGFPE) are caught and reported.
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RHEA_BIN = ROOT / "bin" / "rhea"
TIMEOUT_SEC = 10.0

# Tuple format:
# (suite_name, source_code, stdin_data, expected_rc, expected_stdout, expected_err_code)
DIFFERENTIAL_CASES: list[tuple[str, str, str | None, int, str | None, str]] = [
    (
        "01_shadowing_and_nested_scopes",
        """fn main() {
    x = 10
    {
        let x = 20
        print("b1={x}")
        {
            let x = 30
            print("b2={x}")
        }
        print("b1_after={x}")
    }
    let x = 40
    print("redecl={x}")
}
""",
        None,
        0,
        "b1=20\nb2=30\nb1_after=20\nredecl=40\n",
        "",
    ),
    (
        "02_recursion_and_early_returns",
        """fn classify(n: Int) -> String {
    if n < 0 {
        return "neg"
    }
    if n == 0 {
        return "zero"
    }
    "pos"
}
fn fib(n: Int) -> Int {
    if n <= 1 {
        return n
    }
    return fib(n - 1) + fib(n - 2)
}
fn main() {
    c1 = classify(-5)
    c2 = classify(0)
    c3 = classify(9)
    f8 = fib(8)
    print("{c1},{c2},{c3},{f8}")
}
""",
        None,
        0,
        "neg,zero,pos,21\n",
        "",
    ),
    (
        "03_closures_and_capture_snapshot",
        """fn main() {
    mut base = 10
    add_base = |x: Int| x + base
    base = 500
    scale_new = |x: Int| x + base
    r1 = add_base(5)
    r2 = scale_new(5)
    print("{r1} {r2}")
}
""",
        None,
        0,
        "15 505\n",
        "",
    ),
    (
        "04_generics_functions_and_structs",
        """struct Pair<T> {
    left: T,
    right: T
}
fn id<T>(v: T) -> T {
    v
}
fn head<T>(xs: List<T>) -> T {
    xs[0]
}
fn main() {
    a = id(123)
    b = id("ok")
    h = head([77, 88, 99])
    p = Pair { left: 4, right: 9 }
    s = p.left + p.right
    print("{a} {b} {h} {s}")
}
""",
        None,
        0,
        "123 ok 77 13\n",
        "",
    ),
    (
        "05_ranges_and_int64_max_boundary",
        """fn main() {
    mut s1 = 0
    for i in 1..=5 {
        s1 += i
    }
    mut s2 = 0
    for i in 1..5 {
        s2 += i
    }
    mut empty_cnt = 0
    for i in 5..=3 {
        empty_cnt += 1
    }
    max_i = 9223372036854775807
    mut max_cnt = 0
    for i in max_i..=max_i {
        if i == max_i {
            max_cnt += 1
        }
    }
    print("{s1} {s2} {empty_cnt} {max_cnt}")
}
""",
        None,
        0,
        "15 10 0 1\n",
        "",
    ),
    (
        "06_nested_loops_break_and_continue",
        """fn main() {
    mut total = 0
    for i in 1..=5 {
        if i == 2 {
            continue
        }
        if i == 5 {
            break
        }
        mut j = 0
        while j < 4 {
            j += 1
            if j == 2 {
                continue
            }
            if j == 4 {
                break
            }
            total += i * 10 + j
        }
    }
    print("total={total}")
}
""",
        None,
        0,
        "total=172\n",
        "",
    ),
    (
        "07_interactive_stdin_and_eof",
        """fn main() {
    a = input()
    b = input()
    eof_after = is_eof()
    print("{a}|{b}|{eof_after}")
}
""",
        "first line\nsecond line\n",
        0,
        "first line|second line|true\n",
        "",
    ),
    (
        "08_runtime_list_out_of_bounds_error",
        """fn main() {
    xs = [10, 20, 30]
    print(xs[5])
}
""",
        None,
        1,
        None,
        "E1003",
    ),
    (
        "09_maps_creation_indexing_mutation_and_helpers",
        """fn main() {
    mut scores = {"alice": 90, "bob": 82}
    scores["bob"] = 88
    scores["carol"] = 95
    has_a = has_key(scores, "alice")
    has_z = has_key(scores, "zed")
    ks = keys(scores)
    vs = values(scores)
    b_val = scores["bob"]
    c_val = scores["carol"]
    n = len(scores)
    print("{n} {b_val} {c_val} {has_a} {has_z} {ks} {vs}")
}
""",
        None,
        0,
        "3 88 95 true false [alice, bob, carol] [90, 88, 95]\n",
        "",
    ),
    (
        "10_empty_typed_map_and_list_index_mutation",
        """fn main() {
    let mut m: Map<String, Int> = {}
    m["x"] = 10
    m["y"] = 25
    mut xs = [1, 2, 3]
    xs[1] = 99
    v_sum = m["x"] + m["y"] + xs[1]
    print("{v_sum} {xs}")
}
""",
        None,
        0,
        "134 [1, 99, 3]\n",
        "",
    ),
    (
        "11_map_missing_key_error_code_parity",
        """fn main() {
    m = {"a": 1, "b": 2}
    print(m["nonexistent_key"])
}
""",
        None,
        1,
        None,
        "E1003",
    ),
    (
        "12_collections_sort_reverse_contains",
        """fn main() {
    xs = [40, 10, 30, 20]
    s = sort(xs)
    r = reverse(s)
    c1 = contains(s, 30)
    c2 = contains(s, 99)
    print("{s} {r} {c1} {c2}")
}
""",
        None,
        0,
        "[10, 20, 30, 40] [40, 30, 20, 10] true false\n",
        "",
    ),
    (
        "13_stderr_eprintln_and_formatting",
        """fn main() {
    print("stdout_ok")
    eprintln("stderr_diag_ok")
}
""",
        None,
        0,
        "stdout_ok\n",
        "",
    ),
    (
        "14_arithmetic_precedence_and_boolean_short_circuit",
        """fn side_effect() -> Bool {
    print("MUST_NOT_RUN")
    return true
}
fn main() {
    a = 2 + 3 * 4 - 10 / 2
    b = (2 + 3) * (4 - 1)
    c = 17 % 5
    sc_and = false && side_effect()
    sc_or = true || side_effect()
    print("{a} {b} {c} {sc_and} {sc_or}")
}
""",
        None,
        0,
        "9 15 2 false true\n",
        "",
    ),
    (
        "15_integer_overflow_trap_parity_e1002",
        """fn main() {
    max_i = 9223372036854775807
    overflow = max_i + 1
    print(overflow)
}
""",
        None,
        1,
        None,
        "E1002",
    ),
    (
        "16_division_by_zero_trap_parity_e1001",
        """fn main() {
    x = 100
    z = 0
    print(x / z)
}
""",
        None,
        1,
        None,
        "E1001",
    ),
    (
        "17_top_level_const_in_functions_and_closures",
        """const GLOBAL_FACTOR = 7
const GLOBAL_OFFSET = 3

fn compute(v: Int) -> Int {
    return v * GLOBAL_FACTOR + GLOBAL_OFFSET
}

fn main() {
    cl = |x: Int| x * GLOBAL_FACTOR - GLOBAL_OFFSET
    r1 = compute(5)
    r2 = cl(5)
    print("{r1} {r2}")
}
""",
        None,
        0,
        "38 32\n",
        "",
    ),
    (
        "18_string_ordering_comparisons",
        """fn main() {
    lt = "apple" < "banana"
    le = "apple" <= "apple"
    gt = "zebra" > "mango"
    ge = "cat" >= "dog"
    print("{lt} {le} {gt} {ge}")
}
""",
        None,
        0,
        "true true true false\n",
        "",
    ),
    (
        "19_struct_printing_equality_field_mutation_and_order",
        """struct Vec2 {
    x: Int,
    y: Int
}
fn main() {
    mut v1 = Vec2 { y: 20, x: 10 }
    v2 = Vec2 { x: 10, y: 20 }
    eq1 = (v1 == v2)
    v1.x = 15
    v1.y += 5
    eq2 = (v1 == v2)
    print("{eq1} {eq2} {v1}")
}
""",
        None,
        0,
        "true false Vec2 {x: 15, y: 25}\n",
        "",
    ),
    (
        "20_multi_param_generics_pair_and_map",
        """struct Entry<K, V> {
    key: K,
    val: V
}
fn make_entry<K, V>(k: K, v: V) -> Entry<K, V> {
    return Entry { key: k, val: v }
}
fn main() {
    e = make_entry("port", 8080)
    k = e.key
    v = e.val + 1
    print("{k}:{v} {e}")
}
""",
        None,
        0,
        "port:8081 Entry {key: port, val: 8080}\n",
        "",
    ),
    (
        "21_list_push_pop_and_compound_index_mutation",
        """fn main() {
    mut xs = [10, 20, 30]
    push(xs, 40)
    xs[0] += 5
    last = pop(xs)
    mut m = {"a": 100}
    m["a"] += 25
    mv = m["a"]
    print("{last} {xs} {mv}")
}
""",
        None,
        0,
        "40 [15, 20, 30] 125\n",
        "",
    ),
    (
        "22_negative_and_zero_ranges",
        """fn main() {
    r1 = -3..=2
    r2 = -2..2
    r3 = 0..=0
    r4 = 0..0
    print("{r1} | {r2} | {r3} | {r4}")
}
""",
        None,
        0,
        "[-3, -2, -1, 0, 1, 2] | [-2, -1, 0, 1] | [0] | []\n",
        "",
    ),
    (
        "23_main_exit_code_propagation",
        """fn main() -> Int {
    print("exiting_with_7")
    return 7
}
""",
        None,
        7,
        "exiting_with_7\n",
        "",
    ),
    (
        "24_nested_2d_matrix_index_mutation_and_single_eval",
        """fn bump_row() -> Int {
    print_inline("R:")
    return 1
}
fn bump_col() -> Int {
    print_inline("C:")
    return 2
}
fn main() {
    mut matrix = [[1, 2, 3], [4, 5, 6]]
    matrix[0][1] = 20
    matrix[bump_row()][bump_col()] += 40
    print("{matrix}")
}
""",
        None,
        0,
        "R:C:[[1, 20, 3], [4, 5, 46]]\n",
        "",
    ),
    (
        "25_nested_struct_list_and_nested_struct_mutation",
        """struct Inner {
    score: Int
}
struct Board {
    cells: List<Int>,
    meta: Inner
}
fn main() {
    mut b = Board { cells: [10, 20, 30], meta: Inner { score: 5 } }
    b.cells[1] = 25
    b.cells[2] += 10
    b.meta.score += 15
    c = b.cells
    s = b.meta.score
    print("{c} score={s}")
}
""",
        None,
        0,
        "[10, 25, 40] score=20\n",
        "",
    ),
    (
        "26_nested_mutation_bounds_error_e1003",
        """fn main() {
    mut matrix = [[1, 2], [3, 4]]
    matrix[1][99] = 50
    print(matrix)
}
""",
        None,
        1,
        None,
        "E1003",
    ),
    (
        "27_stdlib_path_env_fs_cross_backend_parity",
        """import std::path
import std::env
import std::fs
fn main() {
    p = std::path.join("/tmp", "rhea_diff27_test.txt")
    base = std::path.basename(p)
    ext = std::path.extension(p)
    std::env.set("RHEA_DIFF27", "ok_v0120")
    ev = std::env.get("RHEA_DIFF27", "none")
    std::fs.write_text(p, "alpha\\n")
    std::fs.append_text(p, "beta\\n")
    ex1 = std::fs.exists(p)
    lines = std::fs.read_lines(p)
    std::fs.remove_file(p)
    ex2 = std::fs.exists(p)
    print("{base}|{ext}|{ev}|{ex1}|{lines}|{ex2}")
}
""",
        None,
        0,
        "rhea_diff27_test.txt|.txt|ok_v0120|true|[alpha, beta]|false\n",
        "",
    ),
    (
        "28_enums_and_pattern_matching_with_payloads",
        """enum ResultCode {
    Ok(Int),
    Err(String)
}
fn describe(r: ResultCode) -> Int {
    out = match r {
        ResultCode::Ok(v) => v * 2,
        ResultCode::Err(msg) => -1,
        _ => 0
    }
    return out
}
fn main() {
    ok_val = ResultCode::Ok(21)
    err_val = ResultCode::Err("timeout")
    a = describe(ok_val)
    b = describe(err_val)
    print("{a},{b}")
}
""",
        None,
        0,
        "42,-1\n",
        "",
    ),
    (
        "29_interactive_try_parse_int_and_float_parity",
        """fn main() {
    r1 = try_parse_int("  -125 ")
    r2 = try_parse_int("bad_int")
    f1 = try_parse_float(" 3.5 ")
    f2 = try_parse_float("bad_flt")
    ok1 = r1["ok"]
    v1 = r1["value"]
    ok2 = r2["ok"]
    fok1 = f1["ok"]
    fv1 = f1["value"]
    fok2 = f2["ok"]
    print("{ok1}|{v1}|{ok2}|{fok1}|{fv1}|{fok2}")
}
""",
        None,
        0,
        "true|-125|false|true|3.5|false\n",
        "",
    ),
    (
        "30_cpp_reserved_keywords_and_empty_delim_split",
        """import std::string
fn compute(template: Int, default: Int) -> Int {
    class = template + default
    namespace = class * 2
    return namespace
}
fn main() {
    new = compute(10, 5)
    chars = std::string.split("aβc", "")
    empty = std::string.split("", "")
    print("{new}|{chars}|{empty}")
}
""",
        None,
        0,
        "30|[a, β, c]|[]\n",
        "",
    ),
    (
        "31_int64_min_literal_and_mod_minus_one_parity",
        """fn main() {
    min_i = -9223372036854775808
    neg_one = -1
    rem = min_i % neg_one
    plus_one = min_i + 1
    print("{min_i}|{rem}|{plus_one}")
}
""",
        None,
        0,
        "-9223372036854775808|0|-9223372036854775807\n",
        "",
    ),
]




def run_cmd_bounded(cmd: list[str], stdin_data: str | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        cwd=str(ROOT),
        input=stdin_data,
        text=True,
        capture_output=True,
        timeout=TIMEOUT_SEC,
    )


def run_differential_suite() -> tuple[int, list[str]]:
    failures: list[str] = []
    passed = 0

    for name, code, stdin_data, expected_rc, expected_stdout, expected_err_code in DIFFERENTIAL_CASES:
        with tempfile.TemporaryDirectory() as tmpdir:
            src_path = Path(tmpdir) / f"{name}.rhea"
            bin_path = Path(tmpdir) / f"{name}_bin"
            src_path.write_text(code, encoding="utf-8")

            try:
                r_interp = run_cmd_bounded([str(RHEA_BIN), "run", str(src_path)], stdin_data)
                r_vm = run_cmd_bounded([str(RHEA_BIN), "vm", str(src_path)], stdin_data)
                r_comp = run_cmd_bounded([str(RHEA_BIN), "compile", str(src_path), "-o", str(bin_path)])
            except subprocess.TimeoutExpired as exc:
                failures.append(f"[{name}] TimeoutExpired ({TIMEOUT_SEC}s): {exc}\nSource:\n{code}")
                continue

            if r_comp.returncode != 0:
                failures.append(
                    f"[{name}] Native compiler failure (rc={r_comp.returncode}):\n{r_comp.stderr}\nSource:\n{code}"
                )
                continue

            try:
                r_native = run_cmd_bounded([str(bin_path)], stdin_data)
            except subprocess.TimeoutExpired as exc:
                failures.append(f"[{name}] Native binary TimeoutExpired ({TIMEOUT_SEC}s): {exc}\nSource:\n{code}")
                continue

            # Check for abnormal signal termination (negative return code in Python subprocess)
            for bname, res in (("interp", r_interp), ("vm", r_vm), ("native", r_native)):
                if res.returncode < 0:
                    failures.append(
                        f"[{name}] Backend {bname} crashed with signal {-res.returncode}!\nSource:\n{code}"
                    )

            if expected_err_code == "":
                if not (r_interp.returncode == expected_rc and r_vm.returncode == expected_rc and r_native.returncode == expected_rc):
                    failures.append(
                        f"[{name}] Exit code mismatch (expected {expected_rc}): "
                        f"interp={r_interp.returncode}, vm={r_vm.returncode}, native={r_native.returncode}\nSource:\n{code}"
                    )
                    continue
                if not (r_interp.stdout == r_vm.stdout == r_native.stdout):
                    failures.append(
                        f"[{name}] Stdout mismatch across backends:\n"
                        f"  interp={r_interp.stdout!r}\n  vm={r_vm.stdout!r}\n  native={r_native.stdout!r}\nSource:\n{code}"
                    )
                    continue
                if expected_stdout is not None and r_interp.stdout != expected_stdout:
                    failures.append(
                        f"[{name}] Stdout did not match independent ground-truth specification:\n"
                        f"  actual={r_interp.stdout!r}\n  expected={expected_stdout!r}\nSource:\n{code}"
                    )
                    continue
                if not (r_interp.stderr == r_vm.stderr == r_native.stderr):
                    failures.append(
                        f"[{name}] Stderr mismatch:\n"
                        f"  interp={r_interp.stderr!r}\n  vm={r_vm.stderr!r}\n  native={r_native.stderr!r}\nSource:\n{code}"
                    )
                    continue
            else:
                if not (r_interp.returncode != 0 and r_vm.returncode != 0 and r_native.returncode != 0):
                    failures.append(
                        f"[{name}] Expected non-zero runtime error exit across all backends: "
                        f"interp={r_interp.returncode}, vm={r_vm.returncode}, native={r_native.returncode}\nSource:\n{code}"
                    )
                    continue
                for bname, res in (("interp", r_interp), ("vm", r_vm), ("native", r_native)):
                    if expected_err_code not in res.stderr:
                        failures.append(
                            f"[{name}] Backend {bname} missing expected error code {expected_err_code} in stderr: {res.stderr!r}"
                        )

            passed += 1

    # Suite 24: Multi-module example `examples/13_modules_generics_closures.rhea` across all 3 backends
    with tempfile.TemporaryDirectory() as tmpdir:
        bin_13 = Path(tmpdir) / "ex13_bin"
        r_i13 = run_cmd_bounded([str(RHEA_BIN), "run", "examples/13_modules_generics_closures.rhea"])
        r_v13 = run_cmd_bounded([str(RHEA_BIN), "vm", "examples/13_modules_generics_closures.rhea"])
        r_c13 = run_cmd_bounded([str(RHEA_BIN), "compile", "examples/13_modules_generics_closures.rhea", "-o", str(bin_13)])
        r_n13 = run_cmd_bounded([str(bin_13)])
        if (
            r_i13.returncode == 0
            and r_v13.returncode == 0
            and r_c13.returncode == 0
            and r_n13.returncode == 0
            and (r_i13.stdout == r_v13.stdout == r_n13.stdout)
            and "50" in r_i13.stdout
        ):
            passed += 1
        else:
            failures.append(
                f"[32_multi_module_example13] Mismatch: i={r_i13.stdout!r} v={r_v13.stdout!r} n={r_n13.stdout!r}"
            )

    return passed, failures


if __name__ == "__main__":
    passed_count, fails = run_differential_suite()
    print(f"[Differential Harness] {passed_count} cross-backend test suites passed ({len(fails)} failures).")
    if fails:
        for f in fails:
            print("FAIL:", f, file=sys.stderr)
        sys.exit(1)
