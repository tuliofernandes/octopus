# Gemma 3 1B

This directory adapts Gemma to Octopus's model-neutral prompt, capability, and
response contracts.

## Quick reference

| Item | Value |
| --- | --- |
| CLI selection | No flag, or `--model gemma` before the mode |
| Local artifact | `./models/gemma-3-1b-it-Q4_K_M.gguf` |
| Current source | `ggml-org/gemma-3-1b-it-GGUF`; exact revision/hash is not yet pinned |
| Default | Yes |
| Verified runtime | Linux CPU and AMD HIP |

## Template contract

- GGUF metadata is preferred through pinned `llama-common` Jinja with the
  model's default reasoning policy.
- If metadata rendering is unavailable or unsupported, the integration uses
  its retained manual Gemma renderer.
- System/developer policy is labeled and folded into the following user turn;
  ordinary user/assistant history order is preserved.
- The manual form uses `<start_of_turn>`, the `model` assistant role, and
  `<end_of_turn>`. That end-of-turn marker is also the textual stop.
- The response parser is pass-through.

## Supported Octopus surface

| Surface | Support |
| --- | --- |
| Text generation and multi-turn history | Yes |
| System/developer policy | Yes, folded into user turns |
| Exposed reasoning | No |
| Tools or structured output | No |
| Vision, audio, or other modalities | Not exposed by Octopus |

## Invocation and fixed policy

```bash
./build/octo
./build/octo --model gemma ask "Say hello."
```

`--model` must precede `ask`. Model path, backend, GPU layers, 512-token output
limit, and deterministic sampling are currently internal; there are no public
flags for them.

## Verification and change checklist

- Contract tests: `test/models/google/gemma/integration_test.cpp`
- Preserve metadata-template behavior and the exact manual fallback.
- Recheck real-artifact policy folding and multi-turn parity when the template
  engine, artifact, or role mapping changes.
