---
name: Bug Report
about: Report a compiler, interpreter, bytecode VM, native codegen, or tooling defect
title: "[Bug]: "
labels: ["bug"]
assignees: []
---

## Summary
A concise description of the bug.

## Execution Backend(s) Affected
- [ ] `rhea check` (Type Checker / Frontend)
- [ ] `rhea run` (Tree-Walk Interpreter)
- [ ] `rhea vm` (Stack Bytecode VM)
- [ ] `rhea compile` (Native C++20 Code Generator)
- [ ] CLI Tooling (`fmt`, `lint`, `debug`, `lsp`, `pkg`, `new`)
- [ ] Native C ABI Runtime (`librhea_native` / `python/rhea_ai`)

## Minimal Reproducible `.rhea` Program
```rhea
fn main() {
    // Paste minimal reproducing code here
}
```

## Actual Behavior
Paste the exact command, exit code, and `stdout`/`stderr` output.

## Expected Behavior
Describe what should happen instead.

## Environment
- OS & Architecture (`uname -srm`):
- C++ Compiler (`c++ --version`):
- Rhea Version (`./bin/rhea version`):
