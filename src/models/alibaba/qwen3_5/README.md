# Qwen3.5-4B

This directory adapts Qwen3.5 to Octopus's model-neutral prompt, capability,
and response contracts.

## Quick reference

| Item | Value |
| --- | --- |
| CLI selection | `--model qwen35` before the mode |
| Local artifact | `./models/Qwen3.5-4B-Q4_K_M.gguf` |
| Approved source | `unsloth/Qwen3.5-4B-GGUF` at `720bb031aae5488eae5d6a78768e6d826662b2ae` |
| Artifact identity | 2,740,937,888 bytes; SHA-256 `00fe7986ff5f6b463e62455821146049db6f9313603938a70800d1fb69ef11a4` |
| Default | No; Gemma remains the no-flag default |
| Verified runtime | Linux CPU for short requests; AMD HIP/RDNA3 including long-context probes |

## Template contract

- The GGUF's `tokenizer.chat_template` is rendered by pinned `llama-common`
  Jinja. Its approved SHA-256 is
  `7f0e529032c25183bcd66c7f238da2d377f43be754a94e2725a58c4e16d2ed67`.
- There is no manual or ChatML fallback. Missing, empty, or unrenderable
  metadata fails the request explicitly.
- All leading system/developer messages are folded into one labeled system
  message. Either role is rejected after user/assistant history begins, and a
  conversation must contain a user query.
- User/assistant history order is preserved. The renderer adds the generation
  prompt and disables thinking, so the assistant prefix ends with:

  ```text
  <|im_start|>assistant
  <think>

  </think>
  ```

- Text stops at `<|im_end|>` or native end-of-generation. The response parser
  is pass-through; do not rely on post-hoc protocol stripping.

## Supported Octopus surface

| Surface | Support |
| --- | --- |
| Text generation and multi-turn history | Yes |
| Leading system/developer policy | Yes, folded into one system message |
| Exposed reasoning | No; thinking is disabled |
| Tools or structured output | No |
| Vision, audio, or other modalities | Not exposed by Octopus |

## Invocation and fixed policy

```bash
./build/octo --model qwen35
./build/octo --model qwen35 ask "Say hello."
```

`--model` must precede `ask`. Model path, backend, GPU layers, 512-token output
limit, deterministic sampling, and reasoning policy are currently internal;
there are no public flags for them. Qwen is not the default because the frozen
CPU long-context promotion gate failed.

## Verification and change checklist

- Contract tests: `test/models/alibaba/qwen3_5/integration_test.cpp`
- Exact-template probe: `octopus_model_template_probe`
- Model-backed completion probe: `octopus_model_completion_probe qwen35 ...`
- Recheck artifact/template identity, role folding, empty-metadata failure,
  marker leakage, cancellation, CPU/HIP behavior, and promotion gates after
  relevant changes.
