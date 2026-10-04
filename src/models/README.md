# Model integration READMEs

Every model integration directory must contain a concise `README.md` that
describes the behavior Octopus implements, not every capability advertised by
the upstream model.

Use this shape:

```text
# <Model family>

## Quick reference
CLI value, artifact path/identity, default status, verified backends.

## Template contract
Template source, role mapping, generation/reasoning policy, stops, fallback
and failure behavior.

## Supported Octopus surface
Short table of exposed and deliberately unexposed capabilities.

## Invocation and fixed policy
Public flags/commands and important non-configurable runtime limits.

## Verification and change checklist
Relevant tests/probes and facts that must be rechecked when this integration
changes.
```

Keep the README close to the code, factual, and easy to scan. Link to shared
project documentation instead of repeating general build instructions. Update
it in the same change as template, capability, artifact, flag, stop, fallback,
or verification-policy changes.
