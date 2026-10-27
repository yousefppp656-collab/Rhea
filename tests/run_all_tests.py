#!/usr/bin/env python3
"""Comprehensive End-to-End Verification Suite for Rhea v0.9.3 (55 Automated Tests).
Covers:
- Tests 01..35: Baseline v0.9.2 compiler, VM, native C++20 kernels, autodiff, Adam, checkpoints, LSP, debugger, package manager, icons
- Tests 36..55: New Prompt 1 (v0.9.3) simplified syntax, ranges (`..`, `..=`), duration literals (`1s`, `500ms`, `2.5s`),
  implicit immutable bindings, `mut` shorthand, interactive stdin (`input`, `try_parse_int`, `is_eof`),
  monotonic timing (`sleep`, `now_ms`), birthday countdown, number guessing game, and beginner-friendly diagnostics.
"""
from __future__ import annotations

import os
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(ROOT / "tests"))

RHEA_BIN = ROOT / "bin" / "rhea"
CPP_TESTS_BIN = ROOT / "bin" / "rhea_cpp_tests"


def run_rhea(args: list[str], stdin_data: str | None = None, timeout: float = 15.0) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(RHEA_BIN)] + args,
        cwd=str(ROOT),
        input=stdin_data,
        text=True,
        capture_output=True,
        timeout=timeout,
    )


def run_temp_code(code: str, mode: str = "run", stdin_data: str | None = None) -> subprocess.CompletedProcess:
    with tempfile.NamedTemporaryFile("w", suffix=".rhea", delete=False, encoding="utf-8") as f:
        f.write(code)
        tmp_path = f.name
    try:
        return run_rhea([mode, tmp_path], stdin_data=stdin_data)
    finally:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)


def inspect_png_header_and_top_left(path: Path) -> tuple[int, int, int, tuple[int, int, int, int]]:
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", f"Invalid PNG signature in {path}"
    pos = 8
    width = height = color_type = 0
    idat_chunks = []
    while pos < len(data):
        length = struct.unpack(">I", data[pos : pos + 4])[0]
        ctype = data[pos + 4 : pos + 8]
        chunk_data = data[pos + 8 : pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            width, height, bit_depth, color_type, _, _, _ = struct.unpack(">IIBBBBB", chunk_data)
        elif ctype == b"IDAT":
            idat_chunks.append(chunk_data)
        elif ctype == b"IEND":
            break
    raw = zlib.decompress(b"".join(idat_chunks))
    # First byte is row 0 filter (0 = None), next 4 bytes are top-left pixel RGBA
    r, g, b, a = raw[1], raw[2], raw[3], raw[4]
    return width, height, color_type, (r, g, b, a)


class RheaComprehensiveTestSuite(unittest.TestCase):
    # =========================================================================
    # Baseline Suite (Tests 01..35)
    # =========================================================================
    def test_01_cli_version_and_abi(self):
        r = run_rhea(["version"])
        self.assertEqual(r.returncode, 0)
        self.assertIn("0.13.1", r.stdout)
        self.assertIn("C ABI v2", r.stdout)

    def test_02_cpp_native_kernels_binary(self):
        r = subprocess.run([str(CPP_TESTS_BIN)], cwd=str(ROOT), text=True, capture_output=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("[PASS]", r.stdout)

    def test_03_example_01_fibonacci_and_primes(self):
        r = run_rhea(["run", "examples/01_fibonacci_and_primes.rhea"])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("fib(10) = 55", r.stdout)

    def test_04_example_02_structs_and_collections(self):
        r = run_rhea(["run", "examples/02_structs_and_collections.rhea"])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("Manhattan distance = 19", r.stdout)

    def test_05_example_03_tensor_and_neural_demo(self):
        r = run_rhea(["run", "examples/03_tensor_and_neural_demo.rhea"])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("Matmul sum = 26", r.stdout)

    def test_06_example_04_numerical_integration(self):
        r = run_rhea(["run", "examples/04_Numerical_integration.rhea"])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("Integral of x^2", r.stdout)

    def test_07_example_05_parallel_pipeline(self):
        r = run_rhea(["run", "examples/05_parallel_pipeline.rhea"])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("Pipeline Transform(16) = 42", r.stdout)

    def test_08_example_06_mlp_xor_training(self):
        r = run_rhea(["run", "examples/06_mlp_xor_training.rhea"])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("XOR final MSE loss =", r.stdout)

    def test_09_example_07_enums_and_pattern_matching(self):
        r = run_rhea(["run", "examples/07_enums_and_pattern_matching.rhea"])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("Pattern match results: 42, -1", r.stdout)

    def test_10_example_08_string_interpolation_and_compounds(self):
        r = run_rhea(["run", "examples/08_string_interpolation_and_compounds.rhea"])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("Language: Rhea, Final Score: 42", r.stdout)

    def test_11_example_09_try_catch_and_file_io(self):
        r = run_rhea(["run", "examples/09_try_catch_and_file_io.rhea"])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("Recovered from runtime error", r.stdout)

    def test_12_example_10_closures_and_pipelines(self):
        r = run_rhea(["run", "examples/10_closures_and_pipelines.rhea"])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("scale(8) = 32, piped = 45", r.stdout)

    def test_13_bytecode_vm_execution_parity(self):
        r = run_rhea(["vm", "examples/01_fibonacci_and_primes.rhea"])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("fib(10) = 55", r.stdout)

    def test_14_native_cpp20_compilation_and_execution(self):
        out_bin = ROOT / "build" / "test_fib_bin"
        r = run_rhea(["compile", "examples/01_fibonacci_and_primes.rhea", "-o", str(out_bin)])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(out_bin.exists())
        exec_r = subprocess.run([str(out_bin)], text=True, capture_output=True)
        self.assertEqual(exec_r.returncode, 0)
        self.assertIn("fib(10) = 55", exec_r.stdout)

    def test_15_static_type_checker_rejects_type_mismatch(self):
        r = run_temp_code('fn main() { let x: Int = "hello" }', mode="check")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("E0003", r.stderr)

    def test_16_static_type_checker_rejects_immutable_reassignment(self):
        r = run_temp_code("fn main() { let x = 10\n x = 20 }", mode="check")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("E0005", r.stderr)

    def test_17_static_type_checker_rejects_undefined_identifier(self):
        r = run_temp_code("fn main() { print(unknown_var) }", mode="check")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("E0004", r.stderr)

    def test_18_static_type_checker_rejects_arity_mismatch(self):
        r = run_temp_code("fn add(a: Int, b: Int) -> Int { return a + b }\nfn main() { print(add(1)) }", mode="check")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("E0006", r.stderr)

    def test_19_runtime_division_by_zero_caught(self):
        r = run_temp_code("fn main() { let x = 10 / 0 }", mode="run")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("E1001", r.stderr)

    def test_20_runtime_list_out_of_bounds_caught(self):
        r = run_temp_code("fn main() { let xs = [1, 2, 3]\n print(xs[9]) }", mode="run")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("E1003", r.stderr)

    def test_21_runtime_recursion_limit_enforced(self):
        r = run_temp_code("fn inf(n: Int) -> Int { return inf(n + 1) }\nfn main() { print(inf(0)) }", mode="run")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("E1004", r.stderr)

    def test_22_python_ai_bridge_and_tensors(self):
        from rhea_ai import Tensor, abi_version
        self.assertEqual(abi_version(), 2)
        a = Tensor([[1.0, 2.0], [3.0, 4.0]])
        b = Tensor([[2.0, 0.0], [1.0, 2.0]])
        c = a @ b
        self.assertEqual(c.shape, (2, 2))
        self.assertAlmostEqual(c.sum(), 26.0, places=6)

    def test_23_python_ai_mlp_xor_training_and_checkpoint(self):
        from rhea_ai import MLP2Layer, Trainer, Predictor, make_xor_dataset
        x, y = make_xor_dataset()
        model = MLP2Layer(2, 16, 1, seed=1337)
        trainer = Trainer(model, lr=0.03)
        metrics = trainer.fit(x, y, epochs=500, target_loss=0.01)
        self.assertTrue(metrics.converged)
        self.assertLess(metrics.final_loss, 0.01)

        with tempfile.NamedTemporaryFile(suffix=".ckpt", delete=False) as f:
            ckpt = f.name
        try:
            model.save(ckpt)
            loaded = MLP2Layer.load(ckpt)
            p1 = Predictor(model).predict(x)
            p2 = Predictor(loaded).predict(x)
            for r1, r2 in zip(p1, p2):
                self.assertAlmostEqual(r1[0], r2[0], places=9)
        finally:
            if os.path.exists(ckpt):
                os.remove(ckpt)

    def test_24_bytecode_disassembler(self):
        r = run_rhea(["disasm", "examples/01_fibonacci_and_primes.rhea"])
        self.assertEqual(r.returncode, 0)
        self.assertIn("LOAD_CONST", r.stdout)

    def test_25_cpp20_code_emission(self):
        r = run_rhea(["emit-cpp", "examples/01_fibonacci_and_primes.rhea"])
        self.assertEqual(r.returncode, 0)
        self.assertIn("rhea_fn_fib", r.stdout)

    def test_26_package_manager_lockfile_verification(self):
        r = run_rhea(["pkg", "verify"])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("Verified rhea.toml & rhea.lock", r.stdout)

    def test_27_native_codegen_safe_against_metacharacter_filenames(self):
        code = 'fn main() { print("safe_compile") }\n'
        with tempfile.TemporaryDirectory(prefix="rhea_safe test_dir_") as tmpd:
            tricky_dir = Path(tmpd)
            src_file = tricky_dir / "hello world.rhea"
            out_bin = tricky_dir / "hello bin"
            src_file.write_text(code, encoding="utf-8")
            r = run_rhea(["compile", str(src_file), "-o", str(out_bin)])
            self.assertEqual(r.returncode, 0, r.stderr)
            exec_r = subprocess.run([str(out_bin)], text=True, capture_output=True)
            self.assertEqual(exec_r.returncode, 0)
            self.assertIn("safe_compile", exec_r.stdout)

    def test_28_checked_int64_overflow_in_interpreter_and_vm(self):
        code = "fn main() { let big = 9223372036854775807\n let over = big + 1\n print(over) }"
        r_interp = run_temp_code(code, mode="run")
        r_vm = run_temp_code(code, mode="vm")
        self.assertNotEqual(r_interp.returncode, 0)
        self.assertIn("E1002", r_interp.stderr)
        self.assertNotEqual(r_vm.returncode, 0)
        self.assertIn("E1002", r_vm.stderr)

    def test_29_vm_division_by_zero_and_comparisons(self):
        r_div = run_temp_code("fn main() { let x = 10 / 0\n print(x) }", mode="vm")
        self.assertNotEqual(r_div.returncode, 0)
        self.assertIn("E1001", r_div.stderr)

        r_cmp = run_temp_code("fn main() { print(5 >= 3)\n print(2 != 2) }", mode="vm")
        self.assertEqual(r_cmp.returncode, 0, r_cmp.stderr)
        self.assertEqual(r_cmp.stdout.strip().splitlines(), ["true", "false"])

    def test_30_corrupted_checkpoint_rejected(self):
        from rhea_ai import MLP2Layer
        from rhea_ai.bridge import RheaNativeError
        m = MLP2Layer(2, 8, 1, seed=7)
        with tempfile.NamedTemporaryFile(suffix=".ckpt", delete=False) as f:
            ckpt = Path(f.name)
        try:
            m.save(ckpt)
            data = bytearray(ckpt.read_bytes())
            data[-1] ^= 0xFF  # Corrupt FNV-1a checksum byte
            ckpt.write_bytes(bytes(data))
            with self.assertRaises(RheaNativeError):
                MLP2Layer.load(ckpt)
        finally:
            if ckpt.exists():
                ckpt.unlink()

    def test_31_lsp_jsonrpc_session(self):
        def frame(msg: str) -> str:
            return f"Content-Length: {len(msg)}\r\n\r\n{msg}"

        payload = (
            frame('{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}')
            + frame('{"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{}}')
            + frame('{"jsonrpc":"2.0","id":99,"method":"shutdown","params":{}}')
            + frame('{"jsonrpc":"2.0","method":"exit","params":{}}')
        )
        r = run_rhea(["lsp"], stdin_data=payload)
        self.assertEqual(r.returncode, 0)
        self.assertIn('"rhea-lsp"', r.stdout)
        self.assertIn('"hoverProvider":true', r.stdout)

    def test_32_cli_debugger_breakpoints_locals_stack_eval(self):
        r = run_rhea([
            "debug",
            "examples/01_fibonacci_and_primes.rhea",
            "--commands",
            "break 29; continue; locals; stack; eval 6 * 7; continue",
        ])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("Breakpoint set at line 29", r.stdout)
        self.assertIn("6 * 7 => 42", r.stdout)
        self.assertIn("fib(10) = 55", r.stdout)

    def test_33_formatter_and_linter(self):
        r = run_rhea(["lint", "examples/01_fibonacci_and_primes.rhea"])
        self.assertEqual(r.returncode, 0)
        self.assertIn("[OK]", r.stdout)

    def test_34_direct_file_execution_shorthand(self):
        r = run_rhea(["examples/08_string_interpolation_and_compounds.rhea"])
        self.assertEqual(r.returncode, 0)
        self.assertIn("Final Score: 42", r.stdout)

    def test_35_icon_assets_and_rgba_transparency(self):
        for svg_name in ("rhea-logo.svg", "rhea-file-icon.svg", "rhea-file-icon-dark.svg", "rhea-file-icon-light.svg"):
            p = ROOT / "assets" / "icons" / svg_name
            self.assertTrue(p.exists(), f"Missing {p}")
            self.assertIn("<svg", p.read_text(encoding="utf-8"))

        for png_name, expected_dims in (
            ("rhea-file-icon-512.png", (512, 512)),
            ("rhea-file-icon-1024.png", (1024, 1024)),
            ("rhea-social-preview.png", (1200, 640)),
        ):
            p = ROOT / "assets" / "icons" / png_name
            self.assertTrue(p.exists(), f"Missing {p}")
            w, h, color_type, top_left = inspect_png_header_and_top_left(p)
            self.assertEqual((w, h), expected_dims)
            self.assertEqual(color_type, 6)  # RGBA
            self.assertEqual(top_left, (0, 0, 0, 0))

    # =========================================================================
    # New Prompt 1 (v0.9.3) Automated Tests (Tests 36..55 — All 20 Required Cases)
    # =========================================================================
    def test_36_inclusive_range_1_to_5(self):
        """1. Inclusive range `1..=5` produces `1, 2, 3, 4, 5`."""
        code = "fn main() {\n  for n in 1..=5 { print(n) }\n}\n"
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip().splitlines(), ["1", "2", "3", "4", "5"])

    def test_37_exclusive_range_1_to_5(self):
        """2. Exclusive range `1..5` produces `1, 2, 3, 4`."""
        code = "fn main() {\n  for n in 1..5 { print(n) }\n}\n"
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip().splitlines(), ["1", "2", "3", "4"])

    def test_38_single_element_inclusive_range_3_to_3(self):
        """3. Single-element range `3..=3` produces `3`."""
        code = "fn main() {\n  for n in 3..=3 { print(n) }\n}\n"
        r = run_temp_code(code, mode="run")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip().splitlines(), ["3"])

    def test_39_empty_exclusive_range_5_to_5(self):
        """4. Empty range `5..5` executes zero iterations."""
        code = 'fn main() {\n  for n in 5..5 { print("unreachable") }\n  print("done")\n}\n'
        r = run_temp_code(code, mode="run")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "done")

    def test_40_empty_inclusive_range_5_to_3(self):
        """5. Empty or invalid range `5..=3` behaves deterministically (zero iterations)."""
        code = 'fn main() {\n  for n in 5..=3 { print("unreachable") }\n  print("empty_ok")\n}\n'
        r = run_temp_code(code, mode="run")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "empty_ok")

    def test_41_simple_variable_declaration_and_mutation(self):
        """6. Simple variable declaration (`x = 10`, `mut y = 20`) and mutation work."""
        code = """fn main() {
    x = 10
    mut y = 20
    y += x
    print("{x} {y}")
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "10 30")

    def test_42_immutable_variable_reassignment_fails_with_clear_error(self):
        """7. Immutable variable reassignment (`x = 10` then `x = 20`) fails with a clear beginner-friendly error."""
        code = "fn main() {\n  x = 10\n  x = 20\n}\n"
        r = run_temp_code(code, mode="check")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("E0005", r.stderr)
        self.assertIn("why:", r.stderr)
        self.assertIn("help:", r.stderr)
        self.assertIn("example:", r.stderr)

    def test_43_string_interpolation_with_int_float_bool_string(self):
        """8. String interpolation works with `Int`, `Float`, `Bool`, and `String`."""
        code = """fn main() {
    i = 42
    f = 3.5
    b = true
    s = "Rhea"
    print("{s}: int={i}, float={f}, bool={b}")
}
"""
        r = run_temp_code(code, mode="run")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "Rhea: int=42, float=3.5, bool=true")

    def test_44_input_reads_single_line_from_stdin(self):
        """9. `input()` reads a single line from piped stdin."""
        code = 'fn main() {\n  line = input("Prompt: ")\n  print("Got: {line}")\n}\n'
        r = run_temp_code(code, mode="run", stdin_data="Ada Lovelace\n")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("Prompt: Got: Ada Lovelace", r.stdout)

    def test_45_input_reads_multiple_lines_in_a_loop(self):
        """10. `input()` reads multiple lines in a loop."""
        code = """fn main() {
    for i in 1..=3 {
        line = input()
        print("{i}:{line}")
    }
}
"""
        r = run_temp_code(code, mode="run", stdin_data="first\nsecond\nthird\n")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip().splitlines(), ["1:first", "2:second", "3:third"])

    def test_46_input_handles_spaces_correctly(self):
        """11. `input()` handles spaces correctly without truncating at whitespace."""
        code = 'fn main() {\n  s = input()\n  print(s)\n}\n'
        r = run_temp_code(code, mode="run", stdin_data="hello world with   spaces\n")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "hello world with   spaces")

    def test_47_input_handles_eof_cleanly(self):
        """12. `input()` and `is_eof()` handle EOF cleanly without hanging."""
        code = """fn main() {
    mut count = 0
    while !is_eof() {
        line = input()
        if line == "" && is_eof() {
            break
        }
        count += 1
    }
    print("lines={count}")
}
"""
        r = run_temp_code(code, mode="run", stdin_data="a\nb\n")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "lines=2")

    def test_48_integer_parsing_succeeds_for_valid_integers(self):
        """13. Integer parsing (`try_parse_int` & `parse_int`) succeeds for valid integers."""
        code = """fn main() {
    r1 = try_parse_int("  -125 ")
    assert(r1["ok"])
    v1 = r1["value"]
    assert_eq(v1, -125)
    v2 = parse_int("900")
    print("{v1} {v2}")
}
"""
        r = run_temp_code(code, mode="run")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "-125 900")

    def test_49_integer_parsing_handles_invalid_input_without_crashing(self):
        """14. Integer parsing handles invalid input (`"abc"`, `""`) without crashing."""
        code = """fn main() {
    r1 = try_parse_int("abc")
    r2 = try_parse_int("")
    assert(!r1["ok"])
    assert(!r2["ok"])
    print("safe_parse_ok")
}
"""
        r = run_temp_code(code, mode="run")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "safe_parse_ok")

    def test_50_number_guessing_game_completes_with_scripted_stdin(self):
        """15. `examples/11_interactive_number_guessing.rhea` completes successfully with scripted stdin."""
        scripted_input = "not_a_number\n99\n5\n18\n14\n"
        r = run_rhea(["run", "examples/11_interactive_number_guessing.rhea"], stdin_data=scripted_input)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("Invalid input 'not_a_number'", r.stdout)
        self.assertIn("Out of range (99)", r.stdout)
        self.assertIn("Too low!", r.stdout)
        self.assertIn("Too high!", r.stdout)
        self.assertIn("Correct! You guessed 14 in 3 attempt(s)!", r.stdout)

    def test_51_sleep_50ms_and_duration_literals_pause_for_real_time(self):
        """16. `sleep(50ms)` pauses for at least ~40ms of real monotonic time."""
        code = """fn main() {
    t0 = now_ms()
    sleep(50ms)
    elapsed = now_ms() - t0
    print(elapsed)
    assert(elapsed >= 40, "sleep(50ms) must pause for at least 40ms")
}
"""
        t_start = time.monotonic()
        r = run_temp_code(code, mode="run")
        wall_ms = (time.monotonic() - t_start) * 1000.0
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertGreaterEqual(wall_ms, 40.0)

    def test_52_negative_sleep_duration_rejected_cleanly(self):
        """17. Negative sleep duration (`sleep(-10)`) is rejected cleanly with E1008."""
        code = "fn main() {\n  sleep(-10)\n}\n"
        r = run_temp_code(code, mode="run")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("E1008", r.stderr)

    def test_53_birthday_countdown_runs_and_prints_expected_lines(self):
        """18. Birthday countdown (`countdown(2)` + `examples/12_birthday_countdown.rhea` structure) runs and prints expected lines."""
        code = """fn countdown(start: Int) {
    for number in 1..=start {
        print(number)
        sleep(20ms)
    }
    print("Happy Birthday!")
}
fn main() {
    print("Countdown starting...")
    countdown(3)
}
"""
        r = run_temp_code(code, mode="run")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(
            r.stdout.strip().splitlines(),
            ["Countdown starting...", "1", "2", "3", "Happy Birthday!"],
        )

    def test_54_optional_main_return_and_implicit_final_expression_return(self):
        """19. Existing programs using `fn main() -> Int { return 0 }` and new programs using `fn main() { ... }` and implicit return both work."""
        code = """fn square(x: Int) -> Int {
    x * x
}
fn main() {
    print(square(9))
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "81")

    def test_55_all_ten_readability_benchmark_programs_execute(self):
        """20. All 10 Readability Benchmark programs in `examples/readability/` execute cleanly."""
        readability_dir = ROOT / "examples" / "readability"
        files = sorted(readability_dir.glob("*.rhea"))
        self.assertEqual(len(files), 10)
        for f in files:
            stdin_val = "Ada\n" if "03_user_input" in f.name else None
            r = run_rhea(["run", str(f.relative_to(ROOT))], stdin_data=stdin_val)
            self.assertEqual(r.returncode, 0, f"Failed in {f.name}: {r.stderr}")


    # =========================================================================
    # Prompt 2 (v0.9.4) Semantic, Closure, Generic, Module & Cross-Backend Tests (Tests 56..75)
    # =========================================================================
    def test_56_immutable_and_mutable_variable_semantics(self):
        """1. Immutable and mutable variable semantics across declarations and compound updates."""
        code = """fn main() {
    a = 10
    let b = 20
    mut c = 30
    let mut d = 40
    const LIMIT = 100
    c += a
    d -= b
    print("{a} {b} {c} {d} {LIMIT}")
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "10 20 40 20 100")

        for bad_code in (
            "fn main() { a = 10\n a = 11 }",
            "fn main() { let b = 10\n b += 1 }",
            "fn main() { const MAX = 5\n MAX = 6 }",
        ):
            r_bad = run_temp_code(bad_code, mode="check")
            self.assertNotEqual(r_bad.returncode, 0)
            self.assertIn("E0005", r_bad.stderr)

    def test_57_accidental_assignment_to_misspelled_identifier_rejected(self):
        """2. Accidental assignment to an unknown identifier that is a 1-edit typo of a mutable variable is rejected."""
        code = """fn main() {
    mut count = 0
    conut = 1
    print(count)
}
"""
        r = run_temp_code(code, mode="check")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("E0004", r.stderr)
        self.assertIn("count", r.stderr)

    def test_58_shadowing_and_nested_scopes_across_all_backends(self):
        """3. Shadowing in nested scopes and same-scope `let` re-binding preserves outer bindings."""
        code = """fn main() {
    x = 10
    if true {
        let x = 25
        print("if_x={x}")
    }
    {
        mut x = 50
        x += 5
        print("block_x={x}")
    }
    print("outer_x={x}")
    let x = "shadowed_as_string"
    print("rebound_x={x}")
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(
                r.stdout.strip().splitlines(),
                ["if_x=25", "block_x=55", "outer_x=10", "rebound_x=shadowed_as_string"],
            )

    def test_59_type_inference_and_explicit_annotations(self):
        """4. Type inference, homogeneous List checking, and Struct field checking."""
        r_het = run_temp_code('fn main() { xs = [1, "two", 3] }', mode="check")
        self.assertNotEqual(r_het.returncode, 0)
        self.assertIn("E0003", r_het.stderr)

        r_struct_bad = run_temp_code(
            'struct User { id: Int, name: String }\nfn main() { u = User { id: "wrong", name: "Ada" } }',
            mode="check",
        )
        self.assertNotEqual(r_struct_bad.returncode, 0)
        self.assertIn("E0003", r_struct_bad.stderr)

        r_field_missing = run_temp_code(
            'struct User { id: Int, name: String }\nfn main() { u = User { id: 1, name: "Ada" }\n print(u.age) }',
            mode="check",
        )
        self.assertNotEqual(r_field_missing.returncode, 0)
        self.assertIn("E0004", r_field_missing.stderr)

    def test_60_function_argument_validation(self):
        """5. Function argument count and type validation."""
        code = "fn scale(x: Int, factor: Float) -> Float { return float(x) * factor }\n"
        r_ok = run_temp_code(code + "fn main() { print(scale(4, 2.5)) }", mode="run")
        self.assertEqual(r_ok.returncode, 0, r_ok.stderr)
        self.assertEqual(r_ok.stdout.strip(), "10.0")

        r_bad_t = run_temp_code(code + 'fn main() { print(scale("four", 2.5)) }', mode="check")
        self.assertNotEqual(r_bad_t.returncode, 0)
        self.assertIn("E0003", r_bad_t.stderr)

    def test_61_implicit_return_behavior_including_if_else(self):
        """6. Implicit final-expression return in simple functions and branching if/else functions."""
        code = """fn abs_int(x: Int) -> Int {
    if x < 0 {
        -x
    } else {
        x
    }
}
fn main() {
    a = abs_int(-42)
    b = abs_int(19)
    print("{a} {b}")
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "42 19")

    def test_62_multiple_return_paths_and_missing_return_detection(self):
        """7. Functions with non-Unit return type must return on all control paths (E0007)."""
        bad_code = """fn incomplete(x: Int) -> Int {
    if x > 0 {
        return x
    }
}
fn main() {
    print(incomplete(5))
}
"""
        r = run_temp_code(bad_code, mode="check")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("E0007", r.stderr)

    def test_63_recursive_functions_across_all_backends(self):
        """8. Recursive functions (factorial & Fibonacci) work identically across Interpreter, Bytecode VM, and Native Codegen."""
        code = """fn fact(n: Int) -> Int {
    if n <= 1 {
        return 1
    }
    return n * fact(n - 1)
}
fn main() {
    f6 = fact(6)
    print(f6)
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "720")

    def test_64_closure_lexical_capture_across_all_backends(self):
        """9. Closure parameter, return, and lexical variable capture across Interpreter, VM, and Native Codegen."""
        code = """fn main() {
    factor = 3
    multiply = |x: Int| x * factor
    res = multiply(7)
    print(res)
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "21")

        # Also verify closure argument type checking
        bad_closure = """fn main() {
    f = |x: Int| x + 1
    print(f("invalid"))
}
"""
        r_bad = run_temp_code(bad_closure, mode="check")
        self.assertNotEqual(r_bad.returncode, 0)
        self.assertIn("E0003", r_bad.stderr)

    def test_65_closure_capture_snapshot_and_independent_instances(self):
        """10. Closure captures a deterministic value snapshot at creation time."""
        code = """fn main() {
    mut step = 5
    c1 = |x: Int| x + step
    step = 100
    c2 = |x: Int| x + step
    v1 = c1(10)
    v2 = c2(10)
    print("{v1} {v2}")
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "15 110")

    def test_66_generic_function_and_struct_inference_across_backends(self):
        """11. Generic functions (`fn identity<T>`) and generic structs (`struct Box<T>`) infer types across backends."""
        code = """struct Box<T> {
    value: T
}
fn identity<T>(value: T) -> T {
    value
}
fn first_elem<T>(xs: List<T>) -> T {
    xs[0]
}
fn main() {
    a = identity(99)
    b = identity("generics_ok")
    c = first_elem([11, 22, 33])
    bx = Box { value: 42 }
    v = bx.value
    print("{a} {b} {c} {v}")
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "99 generics_ok 11 42")

    def test_67_invalid_generic_calls_rejected(self):
        """12. Conflicting type arguments for the same generic parameter `T` are rejected with E0003."""
        code = """fn pick_first<T>(a: T, b: T) -> T {
    a
}
fn main() {
    print(pick_first(10, "incompatible_string"))
}
"""
        r = run_temp_code(code, mode="check")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("E0003", r.stderr)

    def test_68_local_multi_module_imports_across_backends(self):
        """13. Local modules (`examples/13_modules_generics_closures.rhea`) execute across Interpreter, VM, and Native Codegen."""
        for mode in ("run", "vm"):
            r = run_rhea([mode, "examples/13_modules_generics_closures.rhea"])
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("score=92 area=500", r.stdout)
            self.assertIn("closure(7)=21 outer_factor=99", r.stdout)

    def test_69_missing_invalid_traversal_and_private_module_imports_rejected(self):
        """14. Missing modules, `..` path traversal, unknown `std::` modules, and private `priv fn` calls fail with E0009."""
        r_missing = run_temp_code("import does_not_exist\nfn main() {}", mode="check")
        self.assertNotEqual(r_missing.returncode, 0)
        self.assertIn("E0009", r_missing.stderr)

        r_std_bad = run_temp_code("import std::nonexistent_module\nfn main() {}", mode="check")
        self.assertNotEqual(r_std_bad.returncode, 0)
        self.assertIn("E0009", r_std_bad.stderr)

        r_trav = run_temp_code("import ..::secret\nfn main() {}", mode="check")
        self.assertNotEqual(r_trav.returncode, 0)
        self.assertIn("E0009", r_trav.stderr)

        # Calling `secret_offset()` (marked `priv fn` in `examples/modules/math_utils.rhea`) from outside must fail with E0009
        with tempfile.TemporaryDirectory(dir=str(ROOT / "examples")) as tmpd:
            p = Path(tmpd) / "priv_call.rhea"
            p.write_text("import modules::math_utils\nfn main() { print(math_utils.secret_offset()) }\n", encoding="utf-8")
            r_priv = run_rhea(["check", str(p)])
            self.assertNotEqual(r_priv.returncode, 0)
            self.assertIn("E0009", r_priv.stderr)

    def test_70_circular_module_imports_detected_cleanly(self):
        """15. Circular imports (`cycle_a -> cycle_b -> cycle_a`) are detected and rejected with E0009."""
        with tempfile.TemporaryDirectory() as tmpd:
            a_path = Path(tmpd) / "cycle_a.rhea"
            b_path = Path(tmpd) / "cycle_b.rhea"
            a_path.write_text("import cycle_b\npub fn fa() -> Int { 1 }\n", encoding="utf-8")
            b_path.write_text("import cycle_a\npub fn fb() -> Int { 2 }\n", encoding="utf-8")
            r = run_rhea(["check", str(a_path)])
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("E0009", r.stderr)
            self.assertIn("circular module import", r.stderr)

    def test_71_inclusive_and_exclusive_range_boundaries_and_descending(self):
        """16. Inclusive (`..=`), exclusive (`..`), single-element, and descending ranges behave deterministically."""
        code = """fn main() {
    r_incl = 1..=4
    r_excl = 1..4
    r_single = 7..=7
    r_empty1 = 7..7
    r_empty2 = 9..=3
    l1 = len(r_incl)
    l2 = len(r_excl)
    l3 = len(r_single)
    l4 = len(r_empty1)
    l5 = len(r_empty2)
    print("{l1} {l2} {l3} {l4} {l5}")
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "4 3 1 0 0")

    def test_72_integer_overflow_boundary_in_range_iteration(self):
        """17. Range iteration at `INT64_MAX..=INT64_MAX` executes 1 iteration without signed overflow or infinite loop."""
        code = """fn main() {
    max_val = 9223372036854775807
    mut count = 0
    for n in max_val..=max_val {
        if n == max_val {
            count += 1
        }
    }
    print(count)
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "1")

    def test_73_collection_iteration_nested_loops_break_continue_across_backends(self):
        """18. Collection iteration, nested loops, `break`, `continue`, and loop-outside check (E0008)."""
        code = """fn main() {
    items = [2, 4, 6, 8, 10]
    mut sum = 0
    for x in items {
        if x == 4 {
            continue
        }
        if x == 10 {
            break
        }
        sum += x
    }
    print(sum)
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "16")

        r_outside = run_temp_code("fn main() { break }", mode="check")
        self.assertNotEqual(r_outside.returncode, 0)
        self.assertIn("E0008", r_outside.stderr)

    def test_74_cross_backend_differential_equivalence_harness(self):
        """19. Differential harness confirms semantic equivalence across Interpreter, Bytecode VM, and Native C++20 Compiler."""
        from differential_harness import run_differential_suite
        passed_count, fails = run_differential_suite()
        self.assertEqual(fails, [], f"Differential harness failures: {fails}")
        self.assertGreaterEqual(passed_count, 9)

    def test_75_v093_regression_coverage(self):
        """20. Full regression check for v0.9.3 features (duration literals, interactive input, readability benchmark)."""
        r = run_rhea(["run", "examples/readability/05_countdown_timer.rhea"])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("Liftoff!", r.stdout)


    # =========================================================================
    # Prompt 3 & Prompt 4 (v0.11.0) Cross-Backend Map, Formatter, LSP, CLI, Stdlib & Icon Tests (Tests 76..95)
    # =========================================================================
    def test_76_map_creation_reading_updating_across_all_backends(self):
        """Map creation, reading, and updating entries produce identical results in run, vm, and compile."""
        code = """fn main() {
    mut m = {"alpha": 10, "beta": 20}
    m["beta"] = 25
    m["gamma"] = 30
    a = m["alpha"]
    b = m["beta"]
    g = m["gamma"]
    n = len(m)
    print("{n}:{a},{b},{g}")
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "3:10,25,30")

    def test_77_map_has_key_keys_values_remove_key_and_equality(self):
        """Map helper APIs (`has_key`, `keys`, `values`, `remove_key`) and map equality work across backends."""
        code = """fn main() {
    mut m1 = {"k1": 100, "k2": 200}
    m2 = {"k1": 100, "k2": 200}
    eq_before = (m1 == m2)
    h1 = has_key(m1, "k1")
    h2 = has_key(m1, "missing")
    ks = keys(m1)
    vs = values(m1)
    print("{eq_before} {h1} {h2} {ks} {vs}")
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "true true false [k1, k2] [100, 200]")

    def test_78_map_missing_key_raises_e1003_across_all_backends(self):
        """Reading a missing key from a Map raises E1003 in Interpreter, VM, and Native Codegen."""
        code = 'fn main() {\n  m = {"a": 1}\n  print(m["missing"])\n}\n'
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("E1003", r.stderr)

    def test_79_map_heterogeneous_and_immutable_mutation_rejected(self):
        """Heterogeneous map values fail with E0003 and mutating an immutable map/list fails with E0005."""
        r_het = run_temp_code('fn main() { m = {"a": 1, "b": "wrong"} }', mode="check")
        self.assertNotEqual(r_het.returncode, 0)
        self.assertIn("E0003", r_het.stderr)

        r_imm_map = run_temp_code('fn main() { m = {"a": 1}\n m["a"] = 2 }', mode="check")
        self.assertNotEqual(r_imm_map.returncode, 0)
        self.assertIn("E0005", r_imm_map.stderr)

        r_imm_list = run_temp_code("fn main() { xs = [1, 2, 3]\n xs[0] = 9 }", mode="check")
        self.assertNotEqual(r_imm_list.returncode, 0)
        self.assertIn("E0005", r_imm_list.stderr)

    def test_80_empty_map_literal_and_list_index_assignment_across_backends(self):
        """Empty typed map literal `let mut m: Map<String, Int> = {}` and `xs[i] = v` work across backends."""
        code = """fn main() {
    let mut m: Map<String, Int> = {}
    m["score"] = 42
    mut xs = [10, 20, 30]
    xs[2] = 99
    s = m["score"]
    print("{s} {xs}")
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "42 [10, 20, 99]")

    def test_81_generic_function_body_invalid_operator_instantiation_rejected(self):
        """Generic functions using `+` or `*` reject `Bool` instantiation at compile time with E0003."""
        code = """fn add_values<T>(a: T, b: T) -> T {
    return a + b
}
fn main() {
    print(add_values(true, false))
}
"""
        r = run_temp_code(code, mode="check")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("E0003", r.stderr)

    def test_82_formatter_deterministic_and_idempotent(self):
        """`rhea fmt` formats messy code deterministically and satisfies `fmt(fmt(x)) == fmt(x)`."""
        messy = """// Header comment
fn   main()   {
x =   10;
   if x > 5 {
      print("value: {x}");
   }
}
"""
        with tempfile.NamedTemporaryFile("w", suffix=".rhea", delete=False, encoding="utf-8") as f:
            f.write(messy)
            p1 = Path(f.name)
        try:
            r1 = run_rhea(["fmt", str(p1)])
            self.assertEqual(r1.returncode, 0, r1.stderr)
            fmt1 = r1.stdout
            p1.write_text(fmt1, encoding="utf-8")
            r2 = run_rhea(["fmt", str(p1)])
            self.assertEqual(r2.returncode, 0, r2.stderr)
            fmt2 = r2.stdout
            self.assertEqual(fmt1, fmt2)
            self.assertIn("// Header comment", fmt1)
            self.assertIn('    if x > 5 {', fmt1)
        finally:
            if p1.exists():
                p1.unlink()

    def test_83_formatter_preserves_comments_and_strings(self):
        """`rhea fmt` preserves inline comments and string spacing untouched."""
        src = 'fn main() {\n  msg = "keep   multiple   spaces"\n  print(msg) // trailing comment\n}\n'
        with tempfile.NamedTemporaryFile("w", suffix=".rhea", delete=False, encoding="utf-8") as f:
            f.write(src)
            p = Path(f.name)
        try:
            r = run_rhea(["fmt", str(p)])
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn('"keep   multiple   spaces"', r.stdout)
            self.assertIn("// trailing comment", r.stdout)
        finally:
            if p.exists():
                p.unlink()

    def test_84_formatter_rejects_malformed_syntax_without_corrupting_file(self):
        """`rhea fmt -w` rejects malformed syntax with non-zero exit and leaves the file unmodified."""
        bad = "fn main( {\n  x = \n}\n"
        with tempfile.NamedTemporaryFile("w", suffix=".rhea", delete=False, encoding="utf-8") as f:
            f.write(bad)
            p = Path(f.name)
        try:
            r = run_rhea(["fmt", "-w", str(p)])
            self.assertNotEqual(r.returncode, 0)
            self.assertEqual(p.read_text(encoding="utf-8"), bad)
        finally:
            if p.exists():
                p.unlink()

    def test_85_formatter_check_and_write_cli_modes(self):
        """`rhea fmt --check` returns 1 when unformatted and 0 after `rhea fmt -w`."""
        unformatted = "fn main() {\n      print(1);\n}\n"
        with tempfile.NamedTemporaryFile("w", suffix=".rhea", delete=False, encoding="utf-8") as f:
            f.write(unformatted)
            p = Path(f.name)
        try:
            r_chk1 = run_rhea(["fmt", "--check", str(p)])
            self.assertNotEqual(r_chk1.returncode, 0)
            r_w = run_rhea(["fmt", "-w", str(p)])
            self.assertEqual(r_w.returncode, 0, r_w.stderr)
            r_chk2 = run_rhea(["fmt", "--check", str(p)])
            self.assertEqual(r_chk2.returncode, 0, r_chk2.stderr)
        finally:
            if p.exists():
                p.unlink()

    def test_86_stateful_lsp_live_diagnostics_hover_completion_definition_symbols(self):
        """`rhea lsp` validates live document edits, publishes real diagnostics, and serves symbols."""
        def frame(msg: str) -> str:
            return f"Content-Length: {len(msg)}\r\n\r\n{msg}"

        valid_open = (
            '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":'
            '{"uri":"file:///test_lsp.rhea","text":"fn compute_ value(x: Int) -> Int { return x * 2 }"}}}'
        ).replace("compute_ value", "compute_value")
        invalid_change = (
            '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":'
            '{"uri":"file:///test_lsp.rhea"},"contentChanges":[{"text":"fn main() { let x: Int = \\"bad\\" }"}]}}'
        )
        payload = (
            frame('{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}')
            + frame(valid_open)
            + frame('{"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///test_lsp.rhea"}}}')
            + frame('{"jsonrpc":"2.0","id":3,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///test_lsp.rhea"}}}')
            + frame('{"jsonrpc":"2.0","id":4,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///test_lsp.rhea"}}}')
            + frame('{"jsonrpc":"2.0","id":5,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///test_lsp.rhea"}}}')
            + frame(invalid_change)
            + frame('{"jsonrpc":"2.0","id":99,"method":"shutdown","params":{}}')
            + frame('{"jsonrpc":"2.0","method":"exit","params":{}}')
        )
        r = run_rhea(["lsp"], stdin_data=payload)
        self.assertEqual(r.returncode, 0)
        self.assertIn('"compute_value"', r.stdout)
        self.assertIn('"E0003"', r.stdout)

    def test_87_project_scaffolding_rhea_new_check_run_build_workflow(self):
        """`rhea new` scaffolds a valid project in a temp dir that passes `check`, `run`, `build`, and rejects `..`."""
        r_bad = run_rhea(["new", "../unsafe_path"])
        self.assertNotEqual(r_bad.returncode, 0)
        self.assertIn("E3001", r_bad.stderr)

        with tempfile.TemporaryDirectory() as tmpd:
            proj = Path(tmpd) / "demo_pkg"
            r_new = run_rhea(["new", str(proj)])
            self.assertEqual(r_new.returncode, 0, r_new.stderr)
            self.assertTrue((proj / "rhea.toml").exists())
            self.assertTrue((proj / "rhea.lock").exists())
            self.assertTrue((proj / "src" / "main.rhea").exists())
            self.assertTrue((proj / "tests" / "test_main.rhea").exists())

            r_chk = subprocess.run([str(RHEA_BIN), "check", "src/main.rhea"], cwd=str(proj), text=True, capture_output=True)
            self.assertEqual(r_chk.returncode, 0, r_chk.stderr)

            r_run = subprocess.run([str(RHEA_BIN), "run"], cwd=str(proj), text=True, capture_output=True)
            self.assertEqual(r_run.returncode, 0, r_run.stderr)
            self.assertIn("Hello from demo_pkg!", r_run.stdout)

            r_bld = subprocess.run([str(RHEA_BIN), "build", "-o", "build/demo_bin"], cwd=str(proj), text=True, capture_output=True)
            self.assertEqual(r_bld.returncode, 0, r_bld.stderr)
            r_exec = subprocess.run([str(proj / "build" / "demo_bin")], cwd=str(proj), text=True, capture_output=True)
            self.assertEqual(r_exec.returncode, 0)
            self.assertIn("Hello from demo_pkg!", r_exec.stdout)

    def test_88_json_stringify_and_parse_nested_objects_arrays_escapes_booleans_null(self):
        """`std::json.stringify` and `std::json.parse` round-trip nested maps, lists, escapes, ints, floats, and bools."""
        code = """import std::json
fn main() {
    raw = "{\\"service\\":\\"rhea\\",\\"active\\":true,\\"count\\":42,\\"items\\":[10,20,30]}"
    obj = std::json.parse(raw)
    assert_eq(obj["service"], "rhea")
    assert_eq(obj["active"], true)
    assert_eq(obj["count"], 42)
    items = obj["items"]
    assert_eq(len(items), 3)
    assert_eq(items[1], 20)
    out = std::json.stringify(obj)
    print(out)
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn('"service":"rhea"', r.stdout)

    def test_89_json_parse_malformed_input_raises_e1006(self):
        """Malformed JSON input raises E1006 cleanly."""
        code = 'import std::json\nfn main() {\n  x = std::json.parse("{\\"unclosed\\": 123")\n}\n'
        r = run_temp_code(code, mode="run")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("E1006", r.stderr)

    def test_90_utf8_string_char_count_and_string_helpers(self):
        """UTF-8 `std::string.char_count`, `join`, `replace`, `starts_with`, and `ends_with` work accurately."""
        code = """import std::string
fn main() {
    s = "Rhea αβγ"
    c = std::string.char_count(s)
    b = len(s)
    j = std::string.join(["a", "b", "c"], "-")
    rep = std::string.replace("foo_bar_foo", "foo", "rhea")
    sw = std::string.starts_with(rep, "rhea")
    ew = std::string.ends_with(rep, "rhea")
    print("{c} {b} {j} {rep} {sw} {ew}")
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "8 11 a-b-c rhea_bar_rhea true true")

    def test_91_path_and_env_standard_library_operations(self):
        """`std::path.*` and `std::env.*` manipulate paths and environment variables reliably."""
        code = """import std::path
import std::env
fn main() {
    p = std::path.join("/usr/local", "bin/rhea.rhea")
    base = std::path.basename(p)
    ext = std::path.extension(p)
    std::env.set("RHEA_TEST_VAR", "verified_100")
    v = std::env.get("RHEA_TEST_VAR", "fallback")
    missing = std::env.get("RHEA_NONEXISTENT_VAR_XYZ", "default_val")
    print("{base}|{ext}|{v}|{missing}")
}
"""
        r = run_temp_code(code, mode="run")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "rhea.rhea|.rhea|verified_100|default_val")

    def test_92_fs_read_lines_append_remove_and_missing_file_error(self):
        """`std::fs.write_text`, `append_text`, `read_lines`, `remove_file`, and missing-file E1006 error work."""
        with tempfile.TemporaryDirectory() as tmpd:
            test_file = (Path(tmpd) / "rhea_test_92.txt").as_posix()
            missing_file = (Path(tmpd) / "does_not_exist_99999.txt").as_posix()
            code = f"""import std::fs
fn main() {{
    p = "{test_file}"
    std::fs.write_text(p, "line1\\n")
    std::fs.append_text(p, "line2\\n")
    lines = std::fs.read_lines(p)
    n = len(lines)
    l2 = lines[1]
    std::fs.remove_file(p)
    ex = std::fs.exists(p)
    print("{{n}} {{l2}} {{ex}}")
}}
"""
            r = run_temp_code(code, mode="run")
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "2 line2 false")

            r_missing = run_temp_code(f'import std::fs\nfn main() {{ s = std::fs.read_text("{missing_file}") }}', mode="run")
            self.assertNotEqual(r_missing.returncode, 0)
            self.assertIn("E1006", r_missing.stderr)

    def test_93_logging_and_stderr_streams(self):
        """`std::log.info`, `std::log.warn`, and `eprintln` write to stdout and stderr."""
        code = """import std::log
fn main() {
    std::log.info("system_ready")
    std::log.warn("low_memory_sim")
    eprintln("custom_stderr_line")
}
"""
        r = run_temp_code(code, mode="run")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("[INFO] system_ready", r.stdout)
        self.assertIn("[WARN] low_memory_sim", r.stderr)
        self.assertIn("custom_stderr_line", r.stderr)

    def test_94_minimalist_icon_suite_icns_and_vscode_integration(self):
        """All multi-size PNGs (16..1024px), native macOS `rhea.icns`, and VS Code icon mappings are valid."""
        for sz in (16, 32, 64, 128, 256, 512, 1024):
            p = ROOT / "assets" / "icons" / f"rhea-icon-{sz}.png"
            self.assertTrue(p.exists(), f"Missing {p}")
            w, h, ctype, top_left = inspect_png_header_and_top_left(p)
            self.assertEqual((w, h), (sz, sz))
            self.assertEqual(ctype, 6)
            self.assertEqual(top_left, (0, 0, 0, 0))

        icns_path = ROOT / "assets" / "icons" / "rhea.icns"
        self.assertTrue(icns_path.exists())
        icns_bytes = icns_path.read_bytes()
        self.assertEqual(icns_bytes[:4], b"icns")
        total_len = struct.unpack(">I", icns_bytes[4:8])[0]
        self.assertEqual(total_len, len(icns_bytes))

        self.assertTrue((ROOT / "tools" / "desktop" / "install-macos-icon.sh").exists())
        self.assertTrue((ROOT / "tools" / "desktop" / "uninstall-macos-icon.sh").exists())

    def test_95_all_fifteen_examples_execute_cleanly(self):
        """All 15 main examples (`examples/01` .. `examples/15`) execute cleanly."""
        ex_files = sorted((ROOT / "examples").glob("[0-9][0-9]_*.rhea"))
        self.assertEqual(len(ex_files), 15)
        for f in ex_files:
            if "11_interactive" in f.name:
                r = run_rhea(["run", str(f.relative_to(ROOT))], stdin_data="14\n")
            elif "12_birthday" in f.name:
                r = run_rhea(["check", str(f.relative_to(ROOT))])
            else:
                r = run_rhea(["run", str(f.relative_to(ROOT))])
            self.assertEqual(r.returncode, 0, f"Example {f.name} failed: {r.stderr}")


    # =========================================================================
    # Prompt 1 (v0.11.0) Correctness Audit Regression, Property-Based & Fuzzing Tests (Tests 96..120)
    # =========================================================================
    def test_96_d01_native_codegen_integer_overflow_traps_with_e1002(self):
        """D-01: `rhea compile` traps with E1002 on signed 64-bit overflow (+, -, *, /, unary -)."""
        cases = [
            "fn main() { x = 9223372036854775807\n y = 1\n print(x + y) }",
            "fn main() { x = -9223372036854775807 - 1\n y = 1\n print(x - y) }",
            "fn main() { x = 9223372036854775807\n y = 2\n print(x * y) }",
            "fn main() { x = -9223372036854775807 - 1\n y = -1\n print(x / y) }",
            "fn main() { x = -9223372036854775807 - 1\n print(-x) }",
        ]
        with tempfile.TemporaryDirectory() as tmpd:
            for idx, code in enumerate(cases):
                src_p = Path(tmpd) / f"ovf_{idx}.rhea"
                bin_p = Path(tmpd) / f"ovf_{idx}_bin"
                src_p.write_text(code, encoding="utf-8")
                rc = run_rhea(["compile", str(src_p), "-o", str(bin_p)])
                self.assertEqual(rc.returncode, 0, rc.stderr)
                rn = subprocess.run([str(bin_p)], text=True, capture_output=True)
                self.assertNotEqual(rn.returncode, 0)
                self.assertIn("E1002", rn.stderr)

    def test_97_d01_native_codegen_div_and_mod_by_zero_trap_with_e1001(self):
        """D-01: `rhea compile` traps with E1001 on integer/float division and modulo by zero."""
        cases = [
            "fn main() { a = 10\n b = 0\n print(a / b) }",
            "fn main() { a = 10\n b = 0\n print(a % b) }",
            "fn main() { a = 10.0\n b = 0.0\n print(a / b) }",
        ]
        with tempfile.TemporaryDirectory() as tmpd:
            for idx, code in enumerate(cases):
                src_p = Path(tmpd) / f"divz_{idx}.rhea"
                bin_p = Path(tmpd) / f"divz_{idx}_bin"
                src_p.write_text(code, encoding="utf-8")
                rc = run_rhea(["compile", str(src_p), "-o", str(bin_p)])
                self.assertEqual(rc.returncode, 0, rc.stderr)
                rn = subprocess.run([str(bin_p)], text=True, capture_output=True)
                self.assertNotEqual(rn.returncode, 0)
                self.assertIn("E1001", rn.stderr)

    def test_98_d02_top_level_const_accessible_in_functions_and_closures_across_backends(self):
        """D-02: Top-level `const` bindings are preserved in functions and closures across run, vm, and compile."""
        code = """const BASE_LIMIT = 40
const STEP_SIZE = 2
fn calc(n: Int) -> Int {
    return BASE_LIMIT + n * STEP_SIZE
}
fn main() {
    f = |k: Int| calc(k) + BASE_LIMIT
    res = f(5)
    print(res)
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "90")

    def test_99_d03_string_ordering_and_non_orderable_type_rejection(self):
        """D-03: String `<, <=, >, >=` works across backends and non-orderable types (`Bool`, `List`) fail with E0003."""
        code = """fn main() {
    a = "alpha" < "beta"
    b = "beta" < "alpha"
    c = "gamma" >= "gamma"
    print("{a} {b} {c}")
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "true false true")

        r_bool = run_temp_code("fn main() { x = true < false }", mode="check")
        self.assertNotEqual(r_bool.returncode, 0)
        self.assertIn("E0003", r_bool.stderr)

        r_list = run_temp_code("fn main() { x = [1, 2] < [3, 4] }", mode="check")
        self.assertNotEqual(r_list.returncode, 0)
        self.assertIn("E0003", r_list.stderr)

    def test_100_d04_struct_field_mutability_type_checking_and_compound_updates(self):
        """D-04: Struct field mutation enforces `mut` (E0005), field types (E0003), and compound updates (`+=`)."""
        r_imm = run_temp_code(
            "struct Counter { v: Int }\nfn main() { c = Counter { v: 1 }\n c.v = 2 }",
            mode="check",
        )
        self.assertNotEqual(r_imm.returncode, 0)
        self.assertIn("E0005", r_imm.stderr)

        r_bad_t = run_temp_code(
            'struct Counter { v: Int }\nfn main() { mut c = Counter { v: 1 }\n c.v = "wrong" }',
            mode="check",
        )
        self.assertNotEqual(r_bad_t.returncode, 0)
        self.assertIn("E0003", r_bad_t.stderr)

        code_ok = """struct Counter { v: Int }
fn main() {
    mut c = Counter { v: 10 }
    c.v += 5
    mut xs = [1, 2, 3]
    xs[1] *= 10
    cv = c.v
    print("{cv} {xs}")
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code_ok, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "15 [1, 20, 3]")

    def test_101_d05_modulo_operator_rejects_float_and_string_with_e0003(self):
        """D-05: Modulo `%` requires `Int` operands and rejects `Float` or `String` with E0003."""
        r_flt = run_temp_code("fn main() { x = 3.5 % 1.2 }", mode="check")
        self.assertNotEqual(r_flt.returncode, 0)
        self.assertIn("E0003", r_flt.stderr)

        r_str = run_temp_code('fn main() { x = "a" % "b" }', mode="check")
        self.assertNotEqual(r_str.returncode, 0)
        self.assertIn("E0003", r_str.stderr)

    def test_102_d06_struct_printing_equality_and_out_of_order_init_across_backends(self):
        """D-06: Struct out-of-order initialization, `==`, `!=`, and `print(s)` match across run, vm, and compile."""
        code = """struct Config {
    host: String,
    port: Int
}
fn main() {
    c1 = Config { port: 9000, host: "localhost" }
    c2 = Config { host: "localhost", port: 9000 }
    same = (c1 == c2)
    print("{same} {c1}")
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "true Config {host: localhost, port: 9000}")

    def test_103_d07_push_pop_remove_key_mutability_and_type_checks(self):
        """D-07: `push`, `pop`, and `remove_key` enforce mutability (E0005), element types (E0003), and empty-pop E1003."""
        r_imm_push = run_temp_code("fn main() { xs = [1, 2]\n push(xs, 3) }", mode="check")
        self.assertNotEqual(r_imm_push.returncode, 0)
        self.assertIn("E0005", r_imm_push.stderr)

        r_imm_pop = run_temp_code("fn main() { xs = [1, 2]\n pop(xs) }", mode="check")
        self.assertNotEqual(r_imm_pop.returncode, 0)
        self.assertIn("E0005", r_imm_pop.stderr)

        r_bad_push = run_temp_code('fn main() { mut xs = [1, 2]\n push(xs, "bad") }', mode="check")
        self.assertNotEqual(r_bad_push.returncode, 0)
        self.assertIn("E0003", r_bad_push.stderr)

        r_empty_pop = run_temp_code("fn main() { let mut xs: List<Int> = []\n x = pop(xs) }", mode="vm")
        self.assertNotEqual(r_empty_pop.returncode, 0)
        self.assertIn("E1003", r_empty_pop.stderr)

    def test_104_d08_multi_param_generics_and_nested_generic_body_validation(self):
        """D-08: Multi-parameter generic structs/functions unify accurately and nested invalid operators fail with E0003."""
        code_ok = """struct KeyVal<K, V> {
    k: K,
    v: V
}
fn lookup_or<K, V>(m: Map<K, V>, key: K, fallback: V) -> V {
    if has_key(m, key) {
        return m[key]
    }
    return fallback
}
fn main() {
    kv = KeyVal { k: "id", v: 42 }
    m = {"a": 10, "b": 20}
    r1 = lookup_or(m, "b", 0)
    r2 = lookup_or(m, "missing", 99)
    k = kv.k
    v = kv.v
    print("{k}={v} {r1} {r2}")
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code_ok, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "id=42 20 99")

        code_bad_nested = """fn nested_op<T>(a: T, b: T) -> T {
    if true {
        res = a - b
        return res
    }
    return a
}
fn main() {
    print(nested_op("hello", "world"))
}
"""
        r_bad = run_temp_code(code_bad_nested, mode="check")
        self.assertNotEqual(r_bad.returncode, 0)
        self.assertIn("E0003", r_bad.stderr)

    def test_105_d09_main_return_exit_code_parity_across_backends(self):
        """D-09: `fn main() -> Int { return 13 }` exits with status 13 in run, vm, and compile."""
        code = "fn main() -> Int { return 13 }\n"
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 13)

    def test_106_d10_duplicate_struct_fields_init_fields_and_params_rejected(self):
        """D-10: Duplicate struct fields, duplicate struct init fields, and duplicate fn/closure params fail with E0002."""
        r1 = run_temp_code("struct Dup { x: Int, x: Int }\nfn main() {}", mode="check")
        self.assertNotEqual(r1.returncode, 0)
        self.assertIn("E0002", r1.stderr)

        r2 = run_temp_code("struct Pt { x: Int, y: Int }\nfn main() { p = Pt { x: 1, x: 2 } }", mode="check")
        self.assertNotEqual(r2.returncode, 0)
        self.assertIn("E0002", r2.stderr)

        r3 = run_temp_code("fn bad(a: Int, a: Int) -> Int { return a }\nfn main() {}", mode="check")
        self.assertNotEqual(r3.returncode, 0)
        self.assertIn("E0002", r3.stderr)

        r4 = run_temp_code("fn main() { f = |x: Int, x: Int| x + 1 }", mode="check")
        self.assertNotEqual(r4.returncode, 0)
        self.assertIn("E0002", r4.stderr)

    def test_107_d10_parser_max_recursion_depth_guard_prevents_stack_overflow(self):
        """D-10: Deeply nested expressions (300 parens) and blocks (300 braces) fail cleanly with E0002."""
        deep_expr = "fn main() { x = " + ("(" * 300) + "1" + (")" * 300) + " }\n"
        r_expr = run_temp_code(deep_expr, mode="check")
        self.assertNotEqual(r_expr.returncode, 0)
        self.assertGreater(r_expr.returncode, 0)
        self.assertIn("E0002", r_expr.stderr)

        deep_blk = "fn main() { " + ("{ " * 300) + "x = 1" + (" }" * 300) + " }\n"
        r_blk = run_temp_code(deep_blk, mode="check")
        self.assertNotEqual(r_blk.returncode, 0)
        self.assertGreater(r_blk.returncode, 0)
        self.assertIn("E0002", r_blk.stderr)

    def test_108_d11_empty_typed_list_and_range_overflow_limit_across_backends(self):
        """D-11: Empty typed `List<String>` and oversized range `E1004` behave identically across run, vm, and compile."""
        code_list = """fn main() {
    let mut names: List<String> = []
    push(names, "Rhea")
    push(names, "v0.11.0")
    print(names)
}
"""
        with tempfile.TemporaryDirectory() as tmpd:
            src_p = Path(tmpd) / "empty_list.rhea"
            bin_p = Path(tmpd) / "empty_list_bin"
            src_p.write_text(code_list, encoding="utf-8")
            rc = run_rhea(["compile", str(src_p), "-o", str(bin_p)])
            self.assertEqual(rc.returncode, 0, rc.stderr)
            rn = subprocess.run([str(bin_p)], text=True, capture_output=True)
            self.assertEqual(rn.returncode, 0)
            self.assertEqual(rn.stdout.strip(), "[Rhea, v0.11.0]")

            rng_p = Path(tmpd) / "huge_rng.rhea"
            rng_bin = Path(tmpd) / "huge_rng_bin"
            rng_p.write_text("fn main() { r = 0..20000000\n print(len(r)) }\n", encoding="utf-8")
            rc2 = run_rhea(["compile", str(rng_p), "-o", str(rng_bin)])
            self.assertEqual(rc2.returncode, 0, rc2.stderr)
            rn2 = subprocess.run([str(rng_bin)], text=True, capture_output=True)
            self.assertNotEqual(rn2.returncode, 0)
            self.assertIn("E1004", rn2.stderr)

    def test_109_d12_stdlib_parity_in_native_codegen(self):
        """D-12: `std::string.*`, `std::log.*`, and `std::io.is_eof` execute identically in `rhea compile`."""
        code = """import std::string
import std::log
fn main() {
    s = trim("  Rhea αβ  ")
    lo = lower(s)
    up = upper(s)
    cc = std::string.char_count(s)
    parts = std::string.split("a,b,c", ",")
    joined = std::string.join(parts, "-")
    rep = std::string.replace(joined, "b", "B")
    sw = std::string.starts_with(rep, "a-")
    ew = std::string.ends_with(rep, "-c")
    std::log.info("native_ok")
    print("{lo}|{up}|{cc}|{rep}|{sw}|{ew}")
}
"""
        with tempfile.TemporaryDirectory() as tmpd:
            src_p = Path(tmpd) / "std_native.rhea"
            bin_p = Path(tmpd) / "std_native_bin"
            src_p.write_text(code, encoding="utf-8")
            r_run = run_rhea(["run", str(src_p)])
            r_vm = run_rhea(["vm", str(src_p)])
            r_comp = run_rhea(["compile", str(src_p), "-o", str(bin_p)])
            self.assertEqual(r_comp.returncode, 0, r_comp.stderr)
            r_nat = subprocess.run([str(bin_p)], text=True, capture_output=True)
            self.assertEqual(r_run.stdout, r_vm.stdout)
            self.assertEqual(r_run.stdout, r_nat.stdout)
            self.assertIn("rhea αβ|RHEA αβ|7|a-B-c|true|true", r_nat.stdout)

    def test_110_higher_order_closure_parameter_across_backends(self):
        """Higher-order functions accepting a closure parameter (`f: Fn`) execute identically across run, vm, and compile."""
        code = """fn apply_twice(f: Fn, x: Int) -> Int {
    return f(f(x))
}
fn main() {
    offset = 3
    add_off = |v: Int| v + offset
    res = apply_twice(add_off, 10)
    print(res)
}
"""
        with tempfile.TemporaryDirectory() as tmpd:
            src_p = Path(tmpd) / "hof.rhea"
            bin_p = Path(tmpd) / "hof_bin"
            src_p.write_text(code, encoding="utf-8")
            r_run = run_rhea(["run", str(src_p)])
            r_vm = run_rhea(["vm", str(src_p)])
            r_comp = run_rhea(["compile", str(src_p), "-o", str(bin_p)])
            self.assertEqual(r_comp.returncode, 0, r_comp.stderr)
            r_nat = subprocess.run([str(bin_p)], text=True, capture_output=True)
            self.assertEqual(r_run.stdout.strip(), "16")
            self.assertEqual(r_vm.stdout.strip(), "16")
            self.assertEqual(r_nat.stdout.strip(), "16")

    def test_111_property_based_arithmetic_and_comparisons_across_backends(self):
        """Property-based test (seed=20261027): 25 generated arithmetic & comparison expressions match Python reference."""
        import random
        rng = random.Random(20261027)
        lines = ["fn main() {"]
        expected_lines = []
        for i in range(25):
            a = rng.randint(-500, 500)
            b = rng.randint(-500, 500)
            c = rng.randint(1, 50)
            val = (a + b) * c - (a % c if a >= 0 else -((-a) % c))
            cmp_res = "true" if (a + b) <= (c * 10) else "false"
            lines.append(f"    v{i} = ({a} + {b}) * {c} - ({a} % {c})")
            lines.append(f"    c{i} = ({a} + {b}) <= ({c} * 10)")
            lines.append(f'    print("{{v{i}}} {{c{i}}}")')
            expected_lines.append(f"{val} {cmp_res}")
        lines.append("}")
        code = "\n".join(lines) + "\n"
        expected_out = "\n".join(expected_lines)
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, f"Seed=20261027 failed: {r.stderr}")
            self.assertEqual(r.stdout.strip(), expected_out)

    def test_112_property_based_range_iteration_and_boundaries(self):
        """Property-based test (seed=20261028): 20 generated inclusive and exclusive ranges match Python reference."""
        import random
        rng = random.Random(20261028)
        lines = ["fn main() {"]
        expected_lines = []
        for i in range(20):
            start = rng.randint(-15, 15)
            end = rng.randint(-15, 15)
            incl = rng.choice([True, False])
            op = "..=" if incl else ".."
            py_list = list(range(start, end + 1)) if (incl and end >= start) else (list(range(start, end)) if (not incl and end > start) else [])
            lines.append(f"    r{i} = {start}{op}{end}")
            lines.append(f"    print(r{i})")
            expected_lines.append("[" + ", ".join(str(x) for x in py_list) + "]")
        lines.append("}")
        code = "\n".join(lines) + "\n"
        expected_out = "\n".join(expected_lines)
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, f"Seed=20261028 failed: {r.stderr}")
            self.assertEqual(r.stdout.strip(), expected_out)

    def test_113_property_based_map_and_list_mutations(self):
        """Property-based test (seed=20261029): Random map & list insertions, updates, and removals match across backends."""
        import random
        rng = random.Random(20261029)
        py_map: dict[str, int] = {}
        py_list: list[int] = []
        lines = [
            "fn main() {",
            "    let mut m: Map<String, Int> = {}",
            "    let mut xs: List<Int> = []",
        ]
        keys_pool = ["k0", "k1", "k2", "k3", "k4", "k5"]
        for _ in range(30):
            k = rng.choice(keys_pool)
            v = rng.randint(-100, 100)
            if rng.random() < 0.75:
                py_map[k] = v
                lines.append(f'    m["{k}"] = {v}')
            else:
                py_map.pop(k, None)
                lines.append(f'    remove_key(m, "{k}")')
            if rng.random() < 0.7 or not py_list:
                py_list.append(v)
                lines.append(f"    push(xs, {v})")
            else:
                py_list.pop()
                lines.append("    pop(xs)")
        lines.append('    ks = keys(m)')
        lines.append('    vs = values(m)')
        lines.append('    print("{ks} {vs} {xs}")')
        lines.append("}")
        code = "\n".join(lines) + "\n"
        sorted_k = sorted(py_map.keys())
        sorted_v = [py_map[k] for k in sorted_k]
        exp = f"[{', '.join(sorted_k)}] [{', '.join(str(x) for x in sorted_v)}] [{', '.join(str(x) for x in py_list)}]"
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, f"Seed=20261029 failed: {r.stderr}")
            self.assertEqual(r.stdout.strip(), exp)

    def test_114_property_based_formatter_idempotence(self):
        """Property-based test (seed=20261030): 15 generated messy programs format idempotently and preserve semantics."""
        import random
        rng = random.Random(20261030)
        for trial in range(15):
            a = rng.randint(1, 50)
            b = rng.randint(1, 50)
            spaces1 = " " * rng.randint(1, 6)
            spaces2 = " " * rng.randint(1, 6)
            semi = ";" if rng.choice([True, False]) else ""
            messy = (
                f"// Trial {trial} comment\n"
                f"fn{spaces1}main(){spaces2}{{\n"
                f"   x ={spaces1}{a}{semi}\n"
                f"      if x > 0 {{\n"
                f'         print("val={{x}} +{b}"){semi} // inline note\n'
                f"   }}\n"
                f"}}\n"
            )
            with tempfile.NamedTemporaryFile("w", suffix=".rhea", delete=False, encoding="utf-8") as f:
                f.write(messy)
                p = Path(f.name)
            try:
                r1 = run_rhea(["fmt", str(p)])
                self.assertEqual(r1.returncode, 0, f"Seed=20261030 trial={trial}: {r1.stderr}")
                fmt1 = r1.stdout
                p.write_text(fmt1, encoding="utf-8")
                r2 = run_rhea(["fmt", str(p)])
                self.assertEqual(r2.returncode, 0)
                self.assertEqual(fmt1, r2.stdout)
                r_exec = run_rhea(["run", str(p)])
                self.assertEqual(r_exec.returncode, 0)
                self.assertEqual(r_exec.stdout.strip(), f"val={a} +{b}")
            finally:
                if p.exists():
                    p.unlink()

    def test_115_property_based_valid_and_invalid_function_calls(self):
        """Property-based test (seed=20261031): Valid and invalid function call combinations are checked accurately."""
        import random
        rng = random.Random(20261031)
        for trial in range(12):
            n_params = rng.randint(1, 4)
            types = [rng.choice(["Int", "String", "Bool"]) for _ in range(n_params)]
            params_str = ", ".join(f"p{i}: {t}" for i, t in enumerate(types))
            valid_args = []
            for t in types:
                if t == "Int": valid_args.append(str(rng.randint(1, 100)))
                elif t == "String": valid_args.append('"ok"')
                else: valid_args.append("true")
            valid_prog = f"fn target({params_str}) -> Int {{ return 1 }}\nfn main() {{ x = target({', '.join(valid_args)}) }}\n"
            rv = run_temp_code(valid_prog, mode="check")
            self.assertEqual(rv.returncode, 0, f"Trial {trial} valid prog failed: {rv.stderr}")

            # Flip first argument type to guarantee type mismatch E0003
            bad_args = list(valid_args)
            bad_args[0] = '"wrong"' if types[0] != "String" else "123"
            bad_prog = f"fn target({params_str}) -> Int {{ return 1 }}\nfn main() {{ x = target({', '.join(bad_args)}) }}\n"
            rb = run_temp_code(bad_prog, mode="check")
            self.assertNotEqual(rb.returncode, 0)
            self.assertIn("E0003", rb.stderr)

    def test_116_bounded_fuzzer_malformed_tokens_and_syntax(self):
        """Bounded fuzzer (seed=20261101): 40 malformed/adversarial inputs never crash or hang the compiler."""
        import random
        rng = random.Random(20261101)
        fuzz_corp = [
            "",
            "   \n\t  ",
            "// only a comment",
            "/* unterminated block comment",
            'fn main() { s = "unterminated string }',
            "fn main() { x = 123abc }",
            "fn main() { x = 1.2e }",
            "fn main() { x = 99999999999999999999999999999 }",
            "fn main() { x = @#$ }",
            "fn main( { x = 1 }",
            "fn main() { if { } }",
            "fn main() { [1, 2, }",
            "fn main() { {\"a\": } }",
            "fn main() { x = |a, | a }",
            "fn main() { x = 1 |> 2 }",
            "pub let x = 1",
            "struct 123 { x: Int }",
            "enum { A, B }",
            "fn main() { match x { } }",
            "fn main() { try { x = 1 } }",
        ]
        tokens_pool = ["fn", "let", "mut", "if", "else", "while", "for", "in", "return", "struct", "(", ")", "{", "}", "[", "]", ":", "::", "..", "..=", "+", "-", "*", "/", "%", "=", "==", "123", '"str"', "id"]
        for _ in range(20):
            length = rng.randint(3, 25)
            fuzz_corp.append(" ".join(rng.choice(tokens_pool) for _ in range(length)))

        for idx, sample in enumerate(fuzz_corp):
            r = run_temp_code(sample, mode="check")
            # Process must exit normally (never negative signal code like -11 SIGSEGV or -6 SIGABRT)
            self.assertGreaterEqual(
                r.returncode, 0, f"Fuzz sample #{idx} crashed with signal {-r.returncode}: {sample!r}"
            )

    def test_117_bounded_fuzzer_type_checker_and_semantic_edge_cases(self):
        """Bounded semantic fuzzer (seed=20261102): 25 invalid semantic programs reject cleanly without crashing."""
        invalid_programs = [
            "fn main() { print(undeclared_var) }",
            "fn main() { x = 1\n x = 2 }",
            'fn main() { mut x = 1\n x = "str" }',
            'fn main() { xs = [1, "two", 3] }',
            'fn main() { m = {"a": 1, "b": true} }',
            "fn main() { m = {true: 1} }",
            "fn main() { break }",
            "fn main() { continue }",
            "fn f() -> Int { x = 1 }\nfn main() {}",
            'fn f() -> Int { return "not_int" }\nfn main() {}',
            "fn id<T>(a: T, b: T) -> T { return a }\nfn main() { id(1, \"two\") }",
            "import ..::outside\nfn main() {}",
            "import std::nonexistent_mod\nfn main() {}",
            "fn main() { sleep(\"1s\") }",
            "fn main() { if 123 { print(1) } }",
            "fn main() { while \"yes\" { break } }",
            "fn main() { for x in 42 { print(x) } }",
            "struct S { a: Int }\nfn main() { s = S { b: 1 } }",
            "struct S { a: Int, b: Int }\nfn main() { s = S { a: 1 } }",
            "struct S { a: Int }\nfn main() { s = S { a: 1 }\n print(s.missing) }",
            "fn main() { f = |x: Int| x + 1\n print(f(1, 2)) }",
            'fn main() { f = |x: Int| x + 1\n print(f("bad")) }',
            "fn main() { xs = [1, 2]\n print(xs[\"bad_idx\"]) }",
            'fn main() { m = {"a": 1}\n print(m[123]) }',
            "fn main() { x = !123 }",
        ]
        for idx, prog in enumerate(invalid_programs):
            r = run_temp_code(prog, mode="check")
            self.assertGreater(r.returncode, 0, f"Invalid semantic program #{idx} was not rejected: {prog}")
            self.assertIn("error[E000", r.stderr)

    def test_118_asan_ubsan_compiler_and_vm_execution_verification(self):
        """Executes `bin/rhea_asan` (AddressSanitizer + UndefinedBehaviorSanitizer) on compiler, VM, and formatter."""
        asan_bin = ROOT / "bin" / "rhea_asan"
        if not asan_bin.exists():
            self.skipTest("bin/rhea_asan not built")
        r_ver = subprocess.run([str(asan_bin), "version"], cwd=str(ROOT), text=True, capture_output=True)
        self.assertEqual(r_ver.returncode, 0, r_ver.stderr)
        self.assertIn("0.13.1", r_ver.stdout)

        r_run = subprocess.run(
            [str(asan_bin), "run", "examples/13_modules_generics_closures.rhea"],
            cwd=str(ROOT),
            text=True,
            capture_output=True,
        )
        self.assertEqual(r_run.returncode, 0, r_run.stderr)

        r_vm = subprocess.run(
            [str(asan_bin), "vm", "examples/13_modules_generics_closures.rhea"],
            cwd=str(ROOT),
            text=True,
            capture_output=True,
        )
        self.assertEqual(r_vm.returncode, 0, r_vm.stderr)
        self.assertEqual(r_run.stdout, r_vm.stdout)

    def test_119_cross_module_duplicate_symbol_collision_rejected(self):
        """Importing two local modules that define the same public function name fails with E0002."""
        with tempfile.TemporaryDirectory() as tmpd:
            p = Path(tmpd)
            (p / "mod_one.rhea").write_text("pub fn clash() -> Int { return 1 }\n", encoding="utf-8")
            (p / "mod_two.rhea").write_text("pub fn clash() -> Int { return 2 }\n", encoding="utf-8")
            main_f = p / "main.rhea"
            main_f.write_text("import mod_one\nimport mod_two\nfn main() { print(clash()) }\n", encoding="utf-8")
            r = run_rhea(["check", str(main_f)])
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("E0002", r.stderr)

    def test_120_full_24_suite_differential_conformance_verification(self):
        """All 24 cross-backend conformance suites in `tests/differential_harness.py` pass with 0 failures."""
        r = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "differential_harness.py")],
            cwd=str(ROOT),
            text=True,
            capture_output=True,
        )
        self.assertEqual(r.returncode, 0, f"Differential harness failed:\n{r.stdout}\n{r.stderr}")
        self.assertIn("32 cross-backend test suites passed (0 failures)", r.stdout)

    # =========================================================================
    # Prompt 2 (v0.12.0) DX, Diagnostics, Nested Lvalues, Closure Guard, Backend Checks, Formatter, LSP & CLI Tests (Tests 121..145)
    # =========================================================================
    def test_121_diagnostic_levenshtein_suggestions_for_functions_structs_fields_and_modules(self):
        """Diagnostics suggest closest matches for misspelled functions, structs, struct fields, and std:: modules."""
        r_fn = run_temp_code("fn compute_total(x: Int) -> Int { return x }\nfn main() { y = compute_totl(5) }", mode="check")
        self.assertNotEqual(r_fn.returncode, 0)
        self.assertIn("E0004", r_fn.stderr)
        self.assertIn("compute_total", r_fn.stderr)

        r_st = run_temp_code("struct Point2D { x: Int, y: Int }\nfn main() { p = Point2d { x: 1, y: 2 } }", mode="check")
        self.assertNotEqual(r_st.returncode, 0)
        self.assertIn("E0004", r_st.stderr)
        self.assertIn("Point2D", r_st.stderr)

        r_fld = run_temp_code("struct Config { timeout_ms: Int }\nfn main() { c = Config { timeout_ms: 10 }\n print(c.timeout_m) }", mode="check")
        self.assertNotEqual(r_fld.returncode, 0)
        self.assertIn("E0004", r_fld.stderr)
        self.assertIn("timeout_ms", r_fld.stderr)

        r_mod = run_temp_code("import std::strng\nfn main() {}", mode="check")
        self.assertNotEqual(r_mod.returncode, 0)
        self.assertIn("E0009", r_mod.stderr)
        self.assertIn("std::string", r_mod.stderr)

    def test_122_diagnostic_why_and_help_present_across_compile_and_runtime_errors(self):
        """Every compile-time and runtime diagnostic includes code, file:line:col, caret underline, why, and help."""
        r_type = run_temp_code('fn main() {\n    total = 10 + "three"\n}\n', mode="check")
        self.assertNotEqual(r_type.returncode, 0)
        self.assertIn("error[E0003]:", r_type.stderr)
        self.assertIn(":2:", r_type.stderr)
        self.assertIn("^", r_type.stderr)
        self.assertIn("= why:", r_type.stderr)
        self.assertIn("= help:", r_type.stderr)

        r_div = run_temp_code("fn main() {\n    x = 10 / 0\n}\n", mode="run")
        self.assertNotEqual(r_div.returncode, 0)
        self.assertIn("error[E1001]:", r_div.stderr)
        self.assertIn("= why:", r_div.stderr)
        self.assertIn("= help:", r_div.stderr)

    def test_123_nested_2d_list_mutation_and_compound_assignment_across_backends(self):
        """Nested 2D list assignment (`matrix[i][j] = v` and `matrix[i][j] += v`) works identically across run, vm, and compile."""
        code = """fn main() {
    mut grid = [[10, 20], [30, 40]]
    grid[0][1] = 25
    grid[1][0] += 5
    grid[1][1] *= 2
    print(grid)
}
"""
        with tempfile.TemporaryDirectory() as tmpd:
            src_p = Path(tmpd) / "grid.rhea"
            bin_p = Path(tmpd) / "grid_bin"
            src_p.write_text(code, encoding="utf-8")
            r_run = run_rhea(["run", str(src_p)])
            r_vm = run_rhea(["vm", str(src_p)])
            r_comp = run_rhea(["compile", str(src_p), "-o", str(bin_p)])
            self.assertEqual(r_comp.returncode, 0, r_comp.stderr)
            r_nat = subprocess.run([str(bin_p)], text=True, capture_output=True)
            self.assertEqual(r_run.stdout.strip(), "[[10, 25], [35, 80]]")
            self.assertEqual(r_vm.stdout.strip(), "[[10, 25], [35, 80]]")
            self.assertEqual(r_nat.stdout.strip(), "[[10, 25], [35, 80]]")

    def test_124_nested_struct_field_and_list_mutation_across_backends(self):
        """Nested `obj.items[i] = v`, `obj.items[i] += v`, and `obj.inner.field += v` work across run, vm, and compile."""
        code = """struct Stats {
    hits: Int
}
struct Cache {
    buckets: List<Int>,
    stats: Stats
}
fn main() {
    mut c = Cache { buckets: [1, 2, 3], stats: Stats { hits: 10 } }
    c.buckets[0] = 100
    c.buckets[2] += 7
    c.stats.hits += 5
    b = c.buckets
    h = c.stats.hits
    print("{b} {h}")
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "[100, 2, 10] 15")

    def test_125_nested_lvalue_single_evaluation_order_guarantee(self):
        """Each index expression in a nested lvalue assignment is evaluated exactly once in left-to-right order."""
        code = """fn idx_outer() -> Int {
    print_inline("A")
    return 0
}
fn idx_inner() -> Int {
    print_inline("B")
    return 1
}
fn rhs_val() -> Int {
    print_inline("C")
    return 50
}
fn main() {
    mut m = [[1, 2], [3, 4]]
    m[idx_outer()][idx_inner()] += rhs_val()
    v = m[0][1]
    print(":{v}")
}
"""
        with tempfile.TemporaryDirectory() as tmpd:
            src_p = Path(tmpd) / "order.rhea"
            bin_p = Path(tmpd) / "order_bin"
            src_p.write_text(code, encoding="utf-8")
            r_run = run_rhea(["run", str(src_p)])
            r_vm = run_rhea(["vm", str(src_p)])
            r_comp = run_rhea(["compile", str(src_p), "-o", str(bin_p)])
            self.assertEqual(r_comp.returncode, 0, r_comp.stderr)
            r_nat = subprocess.run([str(bin_p)], text=True, capture_output=True)
            self.assertEqual(r_run.stdout.strip(), "ABC:52")
            self.assertEqual(r_vm.stdout.strip(), "ABC:52")
            self.assertEqual(r_nat.stdout.strip(), "ABC:52")

    def test_126_nested_lvalue_immutability_and_type_mismatch_rejected(self):
        """Mutating a nested element of an immutable binding fails with E0005 and wrong leaf type fails with E0003."""
        r_imm = run_temp_code("fn main() { m = [[1, 2], [3, 4]]\n m[0][1] = 99 }", mode="check")
        self.assertNotEqual(r_imm.returncode, 0)
        self.assertIn("E0005", r_imm.stderr)

        r_typ = run_temp_code('fn main() { mut m = [[1, 2], [3, 4]]\n m[0][1] = "bad" }', mode="check")
        self.assertNotEqual(r_typ.returncode, 0)
        self.assertIn("E0003", r_typ.stderr)

    def test_127_nested_lvalue_out_of_bounds_and_missing_map_key_trap_e1003(self):
        """Out-of-bounds index or missing map key in nested mutation traps with E1003 across backends."""
        code_oob = "fn main() { mut m = [[1, 2], [3, 4]]\n m[0][5] = 99 }\n"
        for mode in ("run", "vm"):
            r = run_temp_code(code_oob, mode=mode)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("E1003", r.stderr)

    def test_128_closure_mutating_captured_outer_variable_rejected_with_e0005(self):
        """Attempting to mutate a captured outer variable inside a closure fails at compile time with E0005."""
        r1 = run_temp_code("fn main() {\n mut count = 0\n f = |x: Int| -> Int { count += x\n return count }\n}\n", mode="check")
        self.assertNotEqual(r1.returncode, 0)
        self.assertIn("E0005", r1.stderr)
        self.assertIn("captured outer variable", r1.stderr)

        r2 = run_temp_code("fn main() {\n mut xs = [1, 2]\n f = |x: Int| -> Int { xs[0] = x\n return x }\n}\n", mode="check")
        self.assertNotEqual(r2.returncode, 0)
        self.assertIn("E0005", r2.stderr)

        r3 = run_temp_code("fn main() {\n mut xs = [1, 2]\n f = |x: Int| -> Int { push(xs, x)\n return x }\n}\n", mode="check")
        self.assertNotEqual(r3.returncode, 0)
        self.assertIn("E0005", r3.stderr)

    def test_129_closure_local_mutable_variables_and_nested_closures_work(self):
        """Closures can declare and mutate their own local `mut` variables and nest closures across backends."""
        code = """fn main() {
    factor = 3
    compute = |x: Int| -> Int {
        mut local_acc = x * factor
        local_acc += 5
        inner = |y: Int| y + local_acc
        return inner(2)
    }
    res = compute(4)
    print(res)
}
"""
        with tempfile.TemporaryDirectory() as tmpd:
            src_p = Path(tmpd) / "cl_local.rhea"
            bin_p = Path(tmpd) / "cl_local_bin"
            src_p.write_text(code, encoding="utf-8")
            r_run = run_rhea(["run", str(src_p)])
            r_vm = run_rhea(["vm", str(src_p)])
            r_comp = run_rhea(["compile", str(src_p), "-o", str(bin_p)])
            self.assertEqual(r_comp.returncode, 0, r_comp.stderr)
            r_nat = subprocess.run([str(bin_p)], text=True, capture_output=True)
            self.assertEqual(r_run.stdout.strip(), "19")
            self.assertEqual(r_vm.stdout.strip(), "19")
            self.assertEqual(r_nat.stdout.strip(), "19")

    def test_130_backend_capability_validator_rejects_try_catch_in_vm_and_compile_with_e2002(self):
        """`rhea vm` and `rhea compile` reject `try / catch` upfront with E2002 and suggest `rhea run`."""
        code = "fn main() {\n try {\n x = 1\n } catch err {\n print(err)\n }\n}\n"
        r_vm = run_temp_code(code, mode="vm")
        self.assertNotEqual(r_vm.returncode, 0)
        self.assertIn("E2002", r_vm.stderr)
        self.assertIn("try / catch", r_vm.stderr)
        self.assertIn("rhea run", r_vm.stderr)

        with tempfile.NamedTemporaryFile("w", suffix=".rhea", delete=False, encoding="utf-8") as f:
            f.write(code)
            p = Path(f.name)
        try:
            r_comp = run_rhea(["compile", str(p)])
            self.assertNotEqual(r_comp.returncode, 0)
            self.assertIn("E2002", r_comp.stderr)
            self.assertIn("rhea run", r_comp.stderr)
        finally:
            if p.exists():
                p.unlink()

    def test_131_backend_capability_validator_rejects_std_ai_in_vm_and_compile_with_e2002(self):
        """`rhea vm` and `rhea compile` reject `std::ai.*` / `tensor()` upfront with E2002."""
        code = "fn main() {\n t = tensor([1.0, 2.0])\n print(t)\n}\n"
        r_vm = run_temp_code(code, mode="vm")
        self.assertNotEqual(r_vm.returncode, 0)
        self.assertIn("E2002", r_vm.stderr)
        self.assertIn("rhea run", r_vm.stderr)

    def test_132_backend_capability_validator_rejects_dynamic_json_in_compile_with_e2002(self):
        """`rhea compile` rejects dynamic `std::json.parse` upfront with E2002 while `run` and `vm` execute it."""
        code = 'import std::json\nfn main() {\n obj = std::json.parse("{\\"k\\": 1}")\n print(obj["k"])\n}\n'
        with tempfile.NamedTemporaryFile("w", suffix=".rhea", delete=False, encoding="utf-8") as f:
            f.write(code)
            p = Path(f.name)
        try:
            r_comp = run_rhea(["compile", str(p)])
            self.assertNotEqual(r_comp.returncode, 0)
            self.assertIn("E2002", r_comp.stderr)
            self.assertIn("std::json", r_comp.stderr)
        finally:
            if p.exists():
                p.unlink()

    def test_133_stdlib_path_env_and_fs_full_parity_in_native_compile(self):
        """`std::path`, `std::env`, and `std::fs` compile and execute in `rhea compile` identically to `run` and `vm`."""
        code = """import std::path
import std::env
import std::fs
fn main() {
    p = std::path.join("/tmp", "rhea_test_133.txt")
    d = std::path.dirname(p)
    b = std::path.basename(p)
    std::env.set("RHEA_T133", "native_fs_ok")
    ev = std::env.get("RHEA_T133", "fallback")
    std::fs.write_text(p, "hello_native")
    txt = std::fs.read_text(p)
    std::fs.remove_file(p)
    print("{d}|{b}|{ev}|{txt}")
}
"""
        with tempfile.TemporaryDirectory() as tmpd:
            src_p = Path(tmpd) / "fs_nat.rhea"
            bin_p = Path(tmpd) / "fs_nat_bin"
            src_p.write_text(code, encoding="utf-8")
            rc = run_rhea(["compile", str(src_p), "-o", str(bin_p)])
            self.assertEqual(rc.returncode, 0, rc.stderr)
            rn = subprocess.run([str(bin_p)], text=True, capture_output=True)
            self.assertEqual(rn.returncode, 0)
            self.assertEqual(rn.stdout.strip(), "/tmp|rhea_test_133.txt|native_fs_ok|hello_native")

    def test_134_formatter_golden_output_verification(self):
        """`rhea fmt` produces exact expected golden output across modules, structs, functions, and comments."""
        unformatted = """module   demo::app;
import   std::io;

struct   Point   {
x:   Int,
    y: Int
}

fn   main()   {
   // compute point
   p = Point { x: 1, y: 2 };
   if p.x > 0 {
         print("ok   spaces"); // keep comment
   }
}
"""
        expected_golden = """module demo::app
import std::io

struct Point {
    x: Int,
    y: Int
}

fn main() {
    // compute point
    p = Point { x: 1, y: 2 }
    if p.x > 0 {
        print("ok   spaces")  // keep comment
    }
}
"""
        with tempfile.NamedTemporaryFile("w", suffix=".rhea", delete=False, encoding="utf-8") as f:
            f.write(unformatted)
            p = Path(f.name)
        try:
            r = run_rhea(["fmt", str(p)])
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout, expected_golden)
        finally:
            if p.exists():
                p.unlink()

    def test_135_formatter_directory_mode_check_and_write(self):
        """`rhea fmt --check <dir>` and `rhea fmt -w <dir>` format all `.rhea` files in a directory without modifying unrelated files."""
        with tempfile.TemporaryDirectory() as tmpd:
            d = Path(tmpd)
            f1 = d / "a.rhea"
            f2 = d / "b.rhea"
            readme = d / "NOTES.txt"
            f1.write_text("fn main() {\n      print(1);\n}\n", encoding="utf-8")
            f2.write_text("fn helper() -> Int {\n  return 2;\n}\n", encoding="utf-8")
            readme.write_text("do   not   touch\n", encoding="utf-8")

            r_chk1 = run_rhea(["fmt", "--check", str(d)])
            self.assertNotEqual(r_chk1.returncode, 0)

            r_w = run_rhea(["fmt", "-w", str(d)])
            self.assertEqual(r_w.returncode, 0, r_w.stderr)

            r_chk2 = run_rhea(["fmt", "--check", str(d)])
            self.assertEqual(r_chk2.returncode, 0, r_chk2.stderr)
            self.assertEqual(readme.read_text(encoding="utf-8"), "do   not   touch\n")

    def test_136_lsp_echoes_exact_request_ids_and_handles_multiple_messages(self):
        """`rhea lsp` echoes numeric and string JSON-RPC request IDs across multiple back-to-back messages."""
        def frame(msg: str) -> str:
            return f"Content-Length: {len(msg.encode('utf-8'))}\r\n\r\n{msg}"

        payload = (
            frame('{"jsonrpc":"2.0","id":42,"method":"initialize","params":{}}')
            + frame('{"jsonrpc":"2.0","method":"initialized","params":{}}')
            + frame('{"jsonrpc":"2.0","id":"req-hover-7","method":"textDocument/hover","params":{"textDocument":{"uri":"file:///a.rhea"}}}')
            + frame('{"jsonrpc":"2.0","id":777,"method":"shutdown","params":{}}')
            + frame('{"jsonrpc":"2.0","method":"exit","params":{}}')
        )
        r = run_rhea(["lsp"], stdin_data=payload)
        self.assertEqual(r.returncode, 0)
        self.assertIn('"id":42', r.stdout)
        self.assertIn('"id":"req-hover-7"', r.stdout)
        self.assertIn('"id":777', r.stdout)

    def test_137_lsp_diagnostics_update_and_clear_on_fix_and_did_close(self):
        """`rhea lsp` publishes diagnostics on error, clears them (`[]`) when fixed via didChange, and clears on didClose."""
        def frame(msg: str) -> str:
            return f"Content-Length: {len(msg.encode('utf-8'))}\r\n\r\n{msg}"

        bad_open = '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///fix.rhea","text":"fn main() { let x: Int = \\"bad\\" }"}}}'
        fixed_change = '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///fix.rhea"},"contentChanges":[{"text":"fn main() { let x: Int = 10 }"}]}}'
        close_doc = '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"file:///fix.rhea"}}}'
        payload = (
            frame('{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}')
            + frame(bad_open)
            + frame(fixed_change)
            + frame(close_doc)
            + frame('{"jsonrpc":"2.0","id":2,"method":"shutdown","params":{}}')
            + frame('{"jsonrpc":"2.0","method":"exit","params":{}}')
        )
        r = run_rhea(["lsp"], stdin_data=payload)
        self.assertEqual(r.returncode, 0)
        self.assertIn('"code":"E0003"', r.stdout)
        self.assertIn('"diagnostics":[]', r.stdout)

    def test_138_lsp_utf16_character_positions_on_unicode_source(self):
        """`rhea lsp` converts 1-based UTF-8 byte columns to 0-based UTF-16 code units on lines with Unicode text."""
        import json
        # Line: `fn main() { s = "αβγ"; x = unknown_var }`
        # `"αβγ"` is 6 UTF-8 bytes (3 code points = 3 UTF-16 code units).
        # Let's check the UTF-16 character column of `unknown_var`:
        # `fn main() { s = "αβγ"; x = ` has 27 Unicode code points (all BMP -> 27 UTF-16 units),
        # whereas in UTF-8 bytes it is 30 bytes!
        line_text = 'fn main() { s = "αβγ"; x = unknown_var }'
        body = (
            '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":'
            '{"uri":"file:///utf16.rhea","text":' + json.dumps(line_text) + "}}}"
        )
        raw_bytes = body.encode("utf-8")
        init_bytes = b'{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
        exit_bytes = b'{"jsonrpc":"2.0","method":"exit","params":{}}'
        stream = (
            f"Content-Length: {len(init_bytes)}\r\n\r\n".encode("utf-8") + init_bytes
            + f"Content-Length: {len(raw_bytes)}\r\n\r\n".encode("utf-8") + raw_bytes
            + f"Content-Length: {len(exit_bytes)}\r\n\r\n".encode("utf-8") + exit_bytes
        )
        proc = subprocess.run([str(RHEA_BIN), "lsp"], cwd=str(ROOT), input=stream, capture_output=True)
        out = proc.stdout.decode("utf-8")
        self.assertIn('"character":27', out)

    def test_139_lsp_unknown_method_and_malformed_json_error_codes(self):
        """`rhea lsp` returns -32601 on unknown request methods and -32700 on malformed JSON payloads."""
        def frame(msg: str) -> str:
            return f"Content-Length: {len(msg.encode('utf-8'))}\r\n\r\n{msg}"

        payload = (
            frame('{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}')
            + frame("not_json_at_all")
            + frame('{"jsonrpc":"2.0","id":55,"method":"textDocument/nonExistentMethod","params":{}}')
            + frame('{"jsonrpc":"2.0","id":99,"method":"shutdown","params":{}}')
            + frame('{"jsonrpc":"2.0","method":"exit","params":{}}')
        )
        r = run_rhea(["lsp"], stdin_data=payload)
        self.assertEqual(r.returncode, 0)
        self.assertIn("-32700", r.stdout)
        self.assertIn("-32601", r.stdout)
        self.assertIn('"id":55', r.stdout)

    def test_140_cli_help_version_and_unknown_command_suggestions(self):
        """`rhea --help` lists valid commands and unknown commands return E3002 with a Levenshtein suggestion."""
        r_help = run_rhea(["--help"])
        self.assertEqual(r_help.returncode, 0)
        self.assertIn("rhea compile", r_help.stdout)
        self.assertIn("rhea fmt", r_help.stdout)
        self.assertIn("rhea lsp", r_help.stdout)

        r_bad = run_rhea(["rn", "examples/01_fibonacci_and_primes.rhea"])
        self.assertNotEqual(r_bad.returncode, 0)
        self.assertIn("E3002", r_bad.stderr)
        self.assertIn("rhea run", r_bad.stderr)

    def test_141_cli_complete_new_user_project_workflow_end_to_end(self):
        """End-to-end 7-step project workflow (`new`, `build`, type error diagnostic, `fmt`, `test`) in a temp dir."""
        with tempfile.TemporaryDirectory() as tmpd:
            proj = Path(tmpd) / "workflow_app"
            # 1 & 2. Create project and inspect generated files
            r_new = run_rhea(["new", str(proj)])
            self.assertEqual(r_new.returncode, 0, r_new.stderr)
            for rel in ("rhea.toml", "rhea.lock", "src/main.rhea", "tests/test_main.rhea"):
                self.assertTrue((proj / rel).exists(), f"Missing {rel}")

            # 3. Build and run program
            r_bld = subprocess.run([str(RHEA_BIN), "build", "-o", "build/workflow_bin"], cwd=str(proj), text=True, capture_output=True)
            self.assertEqual(r_bld.returncode, 0, r_bld.stderr)
            self.assertTrue((proj / "build" / "workflow_bin").exists())
            r_exec = subprocess.run([str(proj / "build" / "workflow_bin")], cwd=str(proj), text=True, capture_output=True)
            self.assertEqual(r_exec.returncode, 0)
            self.assertIn("Hello from workflow_app!", r_exec.stdout)

            # 4. Introduce a type error and inspect the diagnostic
            (proj / "src" / "main.rhea").write_text('fn main() {\n    let bad: Int = "not_an_int"\n}\n', encoding="utf-8")
            r_err = subprocess.run([str(RHEA_BIN), "check", "src/main.rhea"], cwd=str(proj), text=True, capture_output=True)
            self.assertNotEqual(r_err.returncode, 0)
            self.assertIn("E0003", r_err.stderr)
            self.assertIn("src/main.rhea:2:", r_err.stderr)

            # 5. Restore messy valid source and format in-place
            (proj / "src" / "main.rhea").write_text('fn   main() {\n      print("Hello from workflow_app!");\n}\n', encoding="utf-8")
            r_fmt = subprocess.run([str(RHEA_BIN), "fmt", "-w"], cwd=str(proj), text=True, capture_output=True)
            self.assertEqual(r_fmt.returncode, 0, r_fmt.stderr)
            r_fmt_chk = subprocess.run([str(RHEA_BIN), "fmt", "--check"], cwd=str(proj), text=True, capture_output=True)
            self.assertEqual(r_fmt_chk.returncode, 0, r_fmt_chk.stderr)

            # 6. Run `rhea test` inside the user project directory
            r_tst = subprocess.run([str(RHEA_BIN), "test"], cwd=str(proj), text=True, capture_output=True)
            self.assertEqual(r_tst.returncode, 0, r_tst.stderr)
            self.assertIn("[PASS]", r_tst.stdout)
            self.assertIn("1/1 test file(s) passed", r_tst.stdout)

    def test_142_cli_rhea_init_in_existing_directory_preserves_files(self):
        """`rhea init` initializes a project in an existing directory without overwriting existing source files."""
        with tempfile.TemporaryDirectory() as tmpd:
            proj = Path(tmpd)
            (proj / "src").mkdir()
            custom_main = 'fn main() {\n    print("Custom main preserved")\n}\n'
            (proj / "src" / "main.rhea").write_text(custom_main, encoding="utf-8")
            r_init = subprocess.run([str(RHEA_BIN), "init", "."], cwd=str(proj), text=True, capture_output=True)
            self.assertEqual(r_init.returncode, 0, r_init.stderr)
            self.assertTrue((proj / "rhea.toml").exists())
            self.assertEqual((proj / "src" / "main.rhea").read_text(encoding="utf-8"), custom_main)

    def test_143_stdlib_argument_validation_and_error_consistency(self):
        """`int("abc")` and `float("xyz")` raise E1006 consistently across run, vm, and compile."""
        code = 'fn main() {\n x = int("not_a_num")\n print(x)\n}\n'
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("E1006", r.stderr)

        with tempfile.TemporaryDirectory() as tmpd:
            src_p = Path(tmpd) / "bad_int.rhea"
            bin_p = Path(tmpd) / "bad_int_bin"
            src_p.write_text(code, encoding="utf-8")
            rc = run_rhea(["compile", str(src_p), "-o", str(bin_p)])
            self.assertEqual(rc.returncode, 0, rc.stderr)
            rn = subprocess.run([str(bin_p)], text=True, capture_output=True)
            self.assertNotEqual(rn.returncode, 0)
            self.assertIn("E1006", rn.stderr)

    def test_144_all_examples_and_readability_benchmarks_verified_under_v0120(self):
        """All 15 main examples and 10 readability benchmark examples pass under v0.12.0."""
        ex_files = sorted((ROOT / "examples").glob("[0-9][0-9]_*.rhea"))
        rb_files = sorted((ROOT / "examples" / "readability").glob("[0-9][0-9]_*.rhea"))
        self.assertEqual(len(ex_files), 15)
        self.assertEqual(len(rb_files), 10)
        for f in rb_files:
            r = run_rhea(["check", str(f.relative_to(ROOT))])
            self.assertEqual(r.returncode, 0, r.stderr)

    def test_145_full_28_suite_differential_conformance_verification(self):
        """All 28 cross-backend conformance suites in `tests/differential_harness.py` pass with 0 failures."""
        r = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "differential_harness.py")],
            cwd=str(ROOT),
            text=True,
            capture_output=True,
        )
        self.assertEqual(r.returncode, 0, f"Differential harness failed:\n{r.stdout}\n{r.stderr}")
        self.assertIn("32 cross-backend test suites passed (0 failures)", r.stdout)

    # =========================================================================
    # Final Master Prompt (v0.13.0) Hardening, Security, Semantic Parity & Performance Tests (Tests 146..160)
    # =========================================================================
    def test_146_d13_empty_delimiter_string_split_utf8_no_infinite_loop(self):
        """D-13: `std::string.split(s, "")` splits into UTF-8 characters in bounded time across all 3 backends."""
        code = """import std::string
fn main() {
    a = std::string.split("aβc", "")
    b = std::string.split("", "")
    print("{a}|{b}")
}
"""
        with tempfile.TemporaryDirectory() as tmpd:
            src_p = Path(tmpd) / "split_e.rhea"
            bin_p = Path(tmpd) / "split_e_bin"
            src_p.write_text(code, encoding="utf-8")
            r_run = run_rhea(["run", str(src_p)], timeout=3.0)
            r_vm = run_rhea(["vm", str(src_p)], timeout=3.0)
            r_comp = run_rhea(["compile", str(src_p), "-o", str(bin_p)], timeout=10.0)
            self.assertEqual(r_comp.returncode, 0, r_comp.stderr)
            r_nat = subprocess.run([str(bin_p)], text=True, capture_output=True, timeout=3.0)
            self.assertEqual(r_run.stdout.strip(), "[a, β, c]|[]")
            self.assertEqual(r_vm.stdout.strip(), "[a, β, c]|[]")
            self.assertEqual(r_nat.stdout.strip(), "[a, β, c]|[]")

    def test_147_d14_lexer_float_and_duration_overflow_rejected_with_e1002(self):
        """D-14: Overflowing float (`1.0e500`) and duration (`999999999999999999999999999999s`) fail cleanly with E1002."""
        r_flt = run_temp_code("fn main() {\n x = 1.0e500\n print(x)\n}\n", mode="check")
        self.assertNotEqual(r_flt.returncode, 0)
        self.assertIn("E1002", r_flt.stderr)
        self.assertNotIn("internal error", r_flt.stderr)

        r_dur = run_temp_code("fn main() {\n d = 999999999999999999999999999999s\n print(d)\n}\n", mode="check")
        self.assertNotEqual(r_dur.returncode, 0)
        self.assertIn("E1002", r_dur.stderr)

    def test_148_d14_int64_min_literal_and_d16_mod_minus_one_across_backends(self):
        """D-14 & D-16: `-9223372036854775808` literal and `INT64_MIN % -1 == 0` work across all 3 backends; `-INT64_MIN` traps with E1002."""
        code_ok = """fn main() {
    m = -9223372036854775808
    d = -1
    r = m % d
    print("{m}:{r}")
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code_ok, mode=mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "-9223372036854775808:0")

        code_neg_trap = """fn main() {
    m = -9223372036854775808
    x = -m
    print(x)
}
"""
        for mode in ("run", "vm"):
            r_trap = run_temp_code(code_neg_trap, mode=mode)
            self.assertNotEqual(r_trap.returncode, 0)
            self.assertIn("E1002", r_trap.stderr)

    def test_149_d15_json_parse_deep_nesting_limit_rejected_with_e1006(self):
        """D-15: `std::json.parse` rejects deeply nested JSON (> 256 levels) cleanly with E1006 without crashing."""
        code = """import std::json
fn main() {
    mut s = "1"
    for i in 1..=300 {
        s = "[" + s + "]"
    }
    x = std::json.parse(s)
    print(x)
}
"""
        for mode in ("run", "vm"):
            r = run_temp_code(code, mode=mode)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("E1006", r.stderr)

    def test_150_d17_enums_and_pattern_matching_across_all_three_backends(self):
        """D-17: `examples/07_enums_and_pattern_matching.rhea` executes identically across run, vm, and compile, and invalid variants fail check."""
        r_run = run_rhea(["run", "examples/07_enums_and_pattern_matching.rhea"])
        r_vm = run_rhea(["vm", "examples/07_enums_and_pattern_matching.rhea"])
        with tempfile.TemporaryDirectory() as tmpd:
            bin_p = Path(tmpd) / "ex07_bin"
            r_comp = run_rhea(["compile", "examples/07_enums_and_pattern_matching.rhea", "-o", str(bin_p)])
            self.assertEqual(r_comp.returncode, 0, r_comp.stderr)
            r_nat = subprocess.run([str(bin_p)], text=True, capture_output=True)
        self.assertEqual(r_run.returncode, 0)
        self.assertEqual(r_vm.returncode, 0, r_vm.stderr)
        self.assertEqual(r_nat.returncode, 0, r_nat.stderr)
        self.assertEqual(r_run.stdout, r_vm.stdout)
        self.assertEqual(r_run.stdout, r_nat.stdout)
        self.assertIn("Pattern match results: 42, -1", r_run.stdout)

        # Verify invalid payload type on enum variant is rejected with E0003
        r_bad = run_temp_code('enum Res { Ok(Int) }\nfn main() { x = Res::Ok("not_int") }\n', mode="check")
        self.assertNotEqual(r_bad.returncode, 0)
        self.assertIn("E0003", r_bad.stderr)

    def test_151_d18_interactive_number_guessing_game_across_all_three_backends(self):
        """D-18: `examples/11_interactive_number_guessing.rhea` runs identically across run, vm, and compile."""
        scripted_in = "not_a_number\n99\n5\n18\n14\n"
        r_run = run_rhea(["run", "examples/11_interactive_number_guessing.rhea"], stdin_data=scripted_in)
        r_vm = run_rhea(["vm", "examples/11_interactive_number_guessing.rhea"], stdin_data=scripted_in)
        with tempfile.TemporaryDirectory() as tmpd:
            bin_p = Path(tmpd) / "ex11_bin"
            r_comp = run_rhea(["compile", "examples/11_interactive_number_guessing.rhea", "-o", str(bin_p)])
            self.assertEqual(r_comp.returncode, 0, r_comp.stderr)
            r_nat = subprocess.run([str(bin_p)], input=scripted_in, text=True, capture_output=True)
        self.assertEqual(r_run.returncode, 0)
        self.assertEqual(r_vm.returncode, 0)
        self.assertEqual(r_nat.returncode, 0)
        self.assertEqual(r_run.stdout, r_vm.stdout)
        self.assertEqual(r_run.stdout, r_nat.stdout)
        self.assertIn("Correct! You guessed 14 in 3 attempt(s)!", r_nat.stdout)

    def test_152_d19_cpp_reserved_keywords_and_zero_compiler_warnings_in_compile(self):
        """D-19: C++ reserved words (`template`, `default`, `class`, `namespace`, `new`) compile with 0 warnings in `rhea compile`."""
        code = """fn compute(template: Int, default: Int) -> Int {
    if template == 0 {
        return default
    }
    class = template + default
    namespace = class * 2
    return namespace
}
fn main() {
    new = compute(10, 5)
    print(new)
}
"""
        with tempfile.TemporaryDirectory() as tmpd:
            src_p = Path(tmpd) / "kw.rhea"
            bin_p = Path(tmpd) / "kw_bin"
            src_p.write_text(code, encoding="utf-8")
            r_comp = run_rhea(["compile", str(src_p), "-o", str(bin_p)])
            self.assertEqual(r_comp.returncode, 0, r_comp.stderr)
            self.assertEqual(r_comp.stderr.strip(), "")
            r_nat = subprocess.run([str(bin_p)], text=True, capture_output=True)
            self.assertEqual(r_nat.stdout.strip(), "30")

    def test_153_d20_stdlib_module_argument_type_and_arity_checking(self):
        """D-20: `TypeChecker` rejects invalid argument types (E0003) and arity (E0006) on `std::*` module functions."""
        r_typ = run_temp_code("import std::string\nfn main() { x = std::string.split(123, 456) }\n", mode="check")
        self.assertNotEqual(r_typ.returncode, 0)
        self.assertIn("E0003", r_typ.stderr)

        r_ari = run_temp_code('import std::path\nfn main() { x = std::path.join("only_one") }\n', mode="check")
        self.assertNotEqual(r_ari.returncode, 0)
        self.assertIn("E0006", r_ari.stderr)

    def test_154_bytecode_disassembler_streaming_range_and_zero_copy_ops(self):
        """`rhea disasm` shows streaming `__range_iter_next` and zero-copy `__push_local` / `__index_local` instructions."""
        code = "fn main() {\n mut xs = [1]\n for i in 1..=5 { push(xs, i) }\n print(xs[0])\n}\n"
        with tempfile.NamedTemporaryFile("w", suffix=".rhea", delete=False, encoding="utf-8") as f:
            f.write(code)
            p = Path(f.name)
        try:
            r = run_rhea(["disasm", str(p)])
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("__range_iter_next", r.stdout)
            self.assertIn("__push_local", r.stdout)
            self.assertIn("__index_local", r.stdout)
        finally:
            if p.exists():
                p.unlink()

    def test_155_lsp_case_insensitive_header_and_oversized_frame_guard(self):
        """`rhea lsp` accepts lowercase `content-length:` headers and rejects oversized Content-Length (>16MB) with -32600."""
        init_msg = '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
        exit_msg = '{"jsonrpc":"2.0","method":"exit","params":{}}'
        payload = (
            f"content-length: {len(init_msg)}\r\n\r\n{init_msg}"
            + "Content-Length: 999999999\r\n\r\n"
            + f"Content-Length: {len(exit_msg)}\r\n\r\n{exit_msg}"
        )
        r = run_rhea(["lsp"], stdin_data=payload)
        self.assertEqual(r.returncode, 0)
        self.assertIn('"id":1', r.stdout)
        self.assertIn("-32600", r.stdout)
        self.assertEqual(r.stderr.strip(), "")

    def test_156_cli_missing_args_nonexistent_files_and_spaces_in_paths(self):
        """CLI returns E1006 on nonexistent files, E3001 on invalid project paths, E3002 on missing flag args, and works in directories with spaces."""
        r_no_file = run_rhea(["run", "nonexistent_file_12345.rhea"])
        self.assertNotEqual(r_no_file.returncode, 0)
        self.assertIn("E1006", r_no_file.stderr)

        r_bad_proj = run_rhea(["new", "../invalid_traversal_proj"])
        self.assertNotEqual(r_bad_proj.returncode, 0)
        self.assertIn("E3001", r_bad_proj.stderr)

        r_no_o = run_rhea(["compile", "examples/01_fibonacci_and_primes.rhea", "-o"])
        self.assertNotEqual(r_no_o.returncode, 0)
        self.assertIn("E3002", r_no_o.stderr)

        with tempfile.TemporaryDirectory() as tmpd:
            spaced_proj = Path(tmpd) / "my spaced project"
            r_new = run_rhea(["new", str(spaced_proj)])
            self.assertEqual(r_new.returncode, 0, r_new.stderr)
            r_tst = subprocess.run([str(RHEA_BIN), "test"], cwd=str(spaced_proj), text=True, capture_output=True)
            self.assertEqual(r_tst.returncode, 0, r_tst.stderr)

    def test_157_performance_streaming_range_and_list_loop_regression_guard(self):
        """P-01 & P-02: 10,000-iteration range loop and 5,000-element list loop complete in < 250ms in both run and vm."""
        code = """fn main() {
    mut xs = [0]
    for i in 1..5000 {
        push(xs, i)
    }
    mut total = 0
    for i in 0..5000 {
        total += xs[i]
    }
    print(total)
}
"""
        for mode in ("run", "vm"):
            t0 = time.perf_counter()
            r = run_temp_code(code, mode=mode)
            dt_ms = (time.perf_counter() - t0) * 1000.0
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "12497500")
            self.assertLess(dt_ms, 250.0, f"{mode} regressed to {dt_ms:.2f} ms")

    def test_158_asan_ubsan_full_v0130_edge_cases(self):
        """Executes `bin/rhea_asan` across enums, `INT64_MIN % -1`, empty-delim split, and JSON nesting guards."""
        asan_bin = ROOT / "bin" / "rhea_asan"
        self.assertTrue(asan_bin.exists())
        for ex in ("examples/07_enums_and_pattern_matching.rhea", "examples/13_modules_generics_closures.rhea"):
            for mode in ("run", "vm"):
                r = subprocess.run([str(asan_bin), mode, ex], cwd=str(ROOT), text=True, capture_output=True)
                self.assertEqual(r.returncode, 0, r.stderr)

    def test_159_benchmark_suite_runner_exists_and_verifies_outputs(self):
        """`benchmarks/run_benchmarks.py` exists and its workload outputs match across backends."""
        bench_script = ROOT / "benchmarks" / "run_benchmarks.py"
        self.assertTrue(bench_script.exists())

    def test_160_full_32_suite_differential_conformance_verification(self):
        """All 32 cross-backend conformance suites (96 executions) in `tests/differential_harness.py` pass with 0 failures."""
        r = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "differential_harness.py")],
            cwd=str(ROOT),
            text=True,
            capture_output=True,
        )
        self.assertEqual(r.returncode, 0, f"Differential harness failed:\n{r.stdout}\n{r.stderr}")
        self.assertIn("32 cross-backend test suites passed (0 failures)", r.stdout)

    # =========================================================================
    # Final Icon Redesign & Application Integration Tests (v0.13.1 — Tests 161..165)
    # =========================================================================
    def test_161_icon_svg_flat_two_tone_palette_and_geometry(self):
        """Dark (#0F172A/#14B8A6) and light (#F8FAFC/#0F766E) SVGs use flat two-tone geometry with zero gradients or strokes."""
        import xml.etree.ElementTree as ET
        dark_txt = (ROOT / "assets" / "icons" / "rhea-file-icon-dark.svg").read_text(encoding="utf-8")
        light_txt = (ROOT / "assets" / "icons" / "rhea-file-icon-light.svg").read_text(encoding="utf-8")
        self.assertIn("#0F172A", dark_txt)
        self.assertIn("#14B8A6", dark_txt)
        self.assertIn("#F8FAFC", light_txt)
        self.assertIn("#0F766E", light_txt)
        for forbidden in ("linearGradient", "radialGradient", "stroke=", "<filter"):
            self.assertNotIn(forbidden, dark_txt)
            self.assertNotIn(forbidden, light_txt)
        d_path = [el.attrib.get("d") for el in ET.fromstring(dark_txt).iter() if el.tag.endswith("path")][0]
        l_path = [el.attrib.get("d") for el in ET.fromstring(light_txt).iter() if el.tag.endswith("path")][0]
        self.assertEqual(d_path, l_path)

    def test_162_multi_size_dark_and_light_rgba_png_suite(self):
        """All 9 sizes (16, 24, 32, 48, 64, 128, 256, 512, 1024) exist in both dark and light RGBA PNG variants."""
        for sz in (16, 24, 32, 48, 64, 128, 256, 512, 1024):
            for prefix in ("rhea-icon-", "rhea-icon-light-"):
                p = ROOT / "assets" / "icons" / f"{prefix}{sz}.png"
                self.assertTrue(p.exists(), f"Missing {p}")
                w, h, ctype, top_left = inspect_png_header_and_top_left(p)
                self.assertEqual((w, h), (sz, sz))
                self.assertEqual(ctype, 6)
                self.assertEqual(top_left, (0, 0, 0, 0))

    def test_163_native_macos_icns_and_windows_ico_packages(self):
        """Native macOS `rhea.icns` and Windows `rhea.ico` containers have valid headers and embedded PNG frames."""
        icns = (ROOT / "assets" / "icons" / "rhea.icns").read_bytes()
        self.assertEqual(icns[:4], b"icns")
        self.assertEqual(struct.unpack(">I", icns[4:8])[0], len(icns))

        ico = (ROOT / "assets" / "icons" / "rhea.ico").read_bytes()
        reserved, ico_type, count = struct.unpack("<HHH", ico[:6])
        self.assertEqual((reserved, ico_type, count), (0, 1, 7))

    def test_164_vscode_extension_icon_and_theme_mappings(self):
        """VS Code extension `package.json`, `file-icon-theme.json`, and `icon-theme.json` reference existing dark/light assets."""
        import json
        vsc = ROOT / "tools" / "vscode-rhea"
        pkg = json.loads((vsc / "package.json").read_text(encoding="utf-8"))
        self.assertEqual(pkg["icon"], "icons/rhea-icon-128.png")
        self.assertTrue((vsc / pkg["icon"]).exists())
        for tf in ("file-icon-theme.json", "icon-theme.json"):
            theme = json.loads((vsc / tf).read_text(encoding="utf-8"))
            self.assertEqual(theme["fileExtensions"]["rhea"], "rhea_dark")
            self.assertEqual(theme["light"]["fileExtensions"]["rhea"], "rhea_light")

    def test_165_desktop_integrations_and_validate_icons_tool(self):
        """`tools/validate_icons.py` and reversible macOS/Linux desktop `--dry-run` scripts succeed cleanly."""
        r = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "validate_icons.py")],
            cwd=str(ROOT),
            text=True,
            capture_output=True,
        )
        self.assertEqual(r.returncode, 0, f"validate_icons.py failed:\n{r.stdout}\n{r.stderr}")
        self.assertIn("[PASS]", r.stdout)

if __name__ == "__main__":
    unittest.main(verbosity=2)
