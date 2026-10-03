# Octopus Product Repository Guidance

This repository contains the Octopus product source. Keep product behavior,
tests, build metadata, and this repository's README coherent in every change.

## Engineering Rules

- Use C++17 and the existing CMake, Conan, Catch2, and formatting patterns.
- Treat memory safety as mandatory. Make ownership and borrowing explicit; use
  RAII; validate bounds, sizes, and nullability; and keep llama.cpp, terminal,
  signal, and backend resource lifetimes obvious.
- Use TDD for behavior changes. Add or update the smallest failing or newly
  meaningful externally observable test before implementation. If the harness
  cannot express the behavior, make the smallest test-enabling change first.
- Preserve user work and avoid unrelated refactors or premature architecture.
- Run the smallest relevant format, build, and test commands before finishing.

## Documentation Sync

Update `README.md` in the same change when user-visible behavior, setup,
prerequisites, commands, executable names, model expectations, or supported
build configurations change.

When work is performed through the canonical Octopus AI workspace, also update
the relevant workspace material in the same task:

- `docs/octopus-project-context.md` for current facts, commands, dependencies,
  tests, and known gaps;
- `docs/octopus-llm-harness.md` for prompt, model-profile, completion, or
  backend-boundary changes;
- `docs/octopus-local-amd-runbook.md` for HIP/Vulkan prerequisites and verified
  results;
- `docs/octopus-roadmap.md` when a capability completes or materially changes;
- eval fixture commits when the audited product revision advances.

After committing product changes and synchronizing the workspace, run from the
workspace root:

```bash
python3 scripts/check_project_sync.py
```

The product commit must exist before the workspace can record its immutable
revision. A temporary mismatch during development is expected; do not finish
the overall task until the cross-repository check passes.
