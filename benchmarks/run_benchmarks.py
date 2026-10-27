#!/usr/bin/env python3
"""Reproducible Cross-Backend Performance Benchmark Suite for Rhea (v0.12.0 vs v0.13.0).

Measures representative workloads across:
  - `rhea run` (Tree-Walking AST Interpreter)
  - `rhea vm` (Stack-Based Bytecode VM)
  - `rhea compile` (Standalone Native C++20 Binary execution, plus AOT compile time)
and verifies exact ground-truth output on every execution.
"""
from __future__ import annotations

import json
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RHEA_BIN = ROOT / "bin" / "rhea"

WORKLOADS: list[tuple[str, str, str]] = [
    (
        "W1_range_arith_10k",
        """fn main() {
    mut acc = 0
    for i in 1..=10000 {
        acc += i * 3 - (i % 7)
    }
    print(acc)
}
""",
        "149985002",
    ),
    (
        "W2_recursion_fib20",
        """fn fib(n: Int) -> Int {
    if n <= 1 {
        return n
    }
    return fib(n - 1) + fib(n - 2)
}
fn main() {
    print(fib(20))
}
""",
        "6765",
    ),
    (
        "W3_list_push_and_index_5k",
        """fn main() {
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
""",
        "12497500",
    ),
    (
        "W4_struct_field_loop_5k",
        """struct Counter {
    x: Int,
    y: Int
}
fn main() {
    mut c = Counter { x: 1, y: 2 }
    for i in 1..=5000 {
        c.x += i
        c.y += (c.x % 7)
    }
    res = c.x + c.y
    print(res)
}
""",
        "12512505",
    ),
    (
        "W5_generics_and_strings_2k",
        """import std::string
fn identity<T>(v: T) -> T {
    return v
}
fn main() {
    mut total_len = 0
    for i in 1..=2000 {
        s = identity("alpha,beta,gamma")
        r = std::string.replace(s, "beta", "delta")
        parts = std::string.split(r, ",")
        joined = std::string.join(parts, "-")
        total_len += len(joined)
    }
    print(total_len)
}
""",
        "34000",
    ),
]


def measure_cmd(cmd: list[str], expected_out: str | None = None, trials: int = 3) -> dict[str, float]:
    # 1 warmup run
    r0 = subprocess.run(cmd, cwd=str(ROOT), text=True, capture_output=True)
    if r0.returncode != 0:
        raise RuntimeError(f"Command {cmd} failed: {r0.stderr}")
    if expected_out is not None and r0.stdout.strip() != expected_out:
        raise RuntimeError(f"Output mismatch for {cmd}: got {r0.stdout.strip()!r}, expected {expected_out!r}")

    times_ms: list[float] = []
    for _ in range(trials):
        t0 = time.perf_counter()
        r = subprocess.run(cmd, cwd=str(ROOT), text=True, capture_output=True)
        dt = (time.perf_counter() - t0) * 1000.0
        if r.returncode != 0:
            raise RuntimeError(f"Command {cmd} failed: {r0.stderr}")
        if expected_out is not None and r.stdout.strip() != expected_out:
            raise RuntimeError(f"Output mismatch for {cmd}: got {r.stdout.strip()!r}, expected {expected_out!r}")
        times_ms.append(dt)

    return {
        "mean_ms": round(statistics.mean(times_ms), 2),
        "stdev_ms": round(statistics.stdev(times_ms) if len(times_ms) > 1 else 0.0, 2),
        "min_ms": round(min(times_ms), 2),
    }


def main() -> None:
    results: dict[str, dict[str, dict[str, float]]] = {}
    ver = subprocess.run([str(RHEA_BIN), "version"], text=True, capture_output=True).stdout.strip()
    print(f"=== Running Rhea Benchmark Suite ({ver}) ===")

    results["CLI_startup_version"] = {
        "cli": measure_cmd([str(RHEA_BIN), "version"], trials=5)
    }

    with tempfile.TemporaryDirectory() as tmpd:
        for name, code, expected in WORKLOADS:
            src_p = Path(tmpd) / f"{name}.rhea"
            bin_p = Path(tmpd) / f"{name}_bin"
            src_p.write_text(code, encoding="utf-8")

            m_run = measure_cmd([str(RHEA_BIN), "run", str(src_p)], expected_out=expected, trials=3)
            m_vm = measure_cmd([str(RHEA_BIN), "vm", str(src_p)], expected_out=expected, trials=3)
            m_comp_time = measure_cmd([str(RHEA_BIN), "compile", str(src_p), "-o", str(bin_p)], trials=3)
            m_native = measure_cmd([str(bin_p)], expected_out=expected, trials=5)

            results[name] = {
                "run": m_run,
                "vm": m_vm,
                "native_exec": m_native,
                "compile_time": m_comp_time,
            }
            print(
                f"{name:28s} | run={m_run['mean_ms']:8.2f} ms | vm={m_vm['mean_ms']:8.2f} ms | "
                f"native={m_native['mean_ms']:6.2f} ms (compile={m_comp_time['mean_ms']:6.2f} ms)"
            )

    if len(sys.argv) > 1:
        Path(sys.argv[1]).write_text(json.dumps({"version": ver, "results": results}, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
