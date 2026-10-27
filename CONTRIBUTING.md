# Contributing to Rhea

Thank you for your interest in contributing to the **Rhea** programming language!

## 1. Development Prerequisites

- A **C++20** compiler (`clang++` 14+ or `g++` 11+, accessible via `c++`)
- **GNU Make** (`make`)
- **Python 3.9+** (`python3`, standard library only)

## 2. Building and Verifying Changes

Before opening a pull request, run the full verification suite from a clean build:

```bash
make clean
make all
make test-asan
./bin/rhea test
python3 tests/differential_harness.py
python3 tools/validate_icons.py
```

## 3. Engineering Standards

1. **Cross-Backend Parity**: Any new language feature or standard library function must either work identically across `rhea run`, `rhea vm`, and `rhea compile` (verified by a test in `tests/differential_harness.py`), or be explicitly guarded in `validate_backend_capabilities()` (`E2002`) and documented in `RHEA_BACKEND_CAPABILITIES.md`.
2. **Checked Arithmetic & Bounds**: Never introduce unguarded signed 64-bit integer arithmetic, unguarded division/modulo, or unbounded recursion in the parser, interpreter, VM, or native code generator.
3. **Diagnostic Clarity**: Every compile-time or runtime error must include an error code (`E0001`–`E3002`), source `Span`, and actionable `why:` / `help:` guidance.
4. **Privacy & Portability**: Never commit absolute local filesystem paths, personal usernames, host-specific build artifacts (`bin/`, `lib/`, `build/`), or credentials.
