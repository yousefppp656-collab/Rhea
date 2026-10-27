## Summary
Describe the changes introduced by this pull request and link any related issues.

## Verification Checklist
- [ ] `make clean && make all` builds with `0` warnings and `0` errors
- [ ] `make test-asan` passes with `0` AddressSanitizer / UndefinedBehaviorSanitizer findings
- [ ] `./bin/rhea test` passes all automated tests
- [ ] `python3 tests/differential_harness.py` passes all cross-backend conformance suites
- [ ] `python3 tools/validate_icons.py` passes (if icon or desktop integration files were touched)
- [ ] Documentation (`README.md`, `RHEA_BACKEND_CAPABILITIES.md`, or `docs/`) updated if user-facing behavior changed
- [ ] No personal filesystem paths, usernames, or secrets are included in any committed file
