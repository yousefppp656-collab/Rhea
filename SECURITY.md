# Security Policy

## Supported Versions

Security fixes and hardening updates are applied to the latest `0.13.x` release line:

| Version | Supported |
|---|---|
| `0.13.x` | Yes |
| `< 0.13.0` | No |

## Reporting a Vulnerability

Please **do not** report security vulnerabilities (such as memory-safety bugs in `librhea_native` or `rhea_driver.cpp`, command-injection vectors in `rhea compile`, path-traversal bypasses in `import` or `rhea pkg`, or denial-of-service crashes in the lexer/parser/LSP) through public GitHub issues.

Instead, please report vulnerabilities privately using **GitHub Private Vulnerability Reporting** (`Security` tab $\rightarrow$ `Report a vulnerability` on the GitHub repository).

### What to Include in a Report

1. **Affected Component**: Specify whether the issue affects the lexer/parser, type checker, interpreter (`rhea run`), bytecode VM (`rhea vm`), native C++20 code generator (`rhea compile`), Language Server (`rhea lsp`), or native tensor runtime (`librhea_native`).
2. **Minimal Reproducer**: Provide a minimal `.rhea` source file, JSON-RPC payload, or C ABI call sequence that reproduces the issue.
3. **Sanitizer Output**: Include output from running `./bin/rhea_asan` or `./bin/rhea_cpp_tests_asan` (`make test-asan`) if applicable.

See [RHEA_SECURITY_AUDIT.md](RHEA_SECURITY_AUDIT.md) for the current security controls, bounds, and sanitizer verification results.
