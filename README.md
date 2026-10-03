# Octopus

Octopus is a local-first conversational AI agent for UNIX/Linux, written in
C++17 and powered by llama.cpp. The current product is a terminal CLI named
`octo` with one-shot and multi-turn conversation modes.

Current capabilities include:

- streamed local-model responses;
- a multi-turn conversation loop;
- one-shot `ask` mode;
- interactive multiline editing, cursor and word movement, deletion, paste,
  Ctrl+C cancellation, and Ctrl+D exit;
- a model-neutral runtime contract for prompt compilation, capabilities,
  inference, and assistant-response parsing;
- an explicitly composed Google Gemma integration with GGUF chat-template
  preference and a manual fallback;
- testable runtime, model-integration, inference, and CLI boundaries that do
  not require a model in unit tests.

Tools, persistence, runtime backend selection, configuration loading, and a
daemon are not implemented yet.

## Prerequisites

- Conan 2 or newer
- CMake 3.25 or newer
- Ninja
- A C++17 compiler
- `clang-format` for the optional `format-check` target

Catch2 3.7.1 is resolved through Conan. llama.cpp is fetched by CMake at the
revision pinned in `CMakeLists.txt`.

## Model

The current default model path is:

```text
./models/gemma-3-1b-it-Q4_K_M.gguf
```

A compatible GGUF can be downloaded with the Hugging Face CLI, for example:

```bash
hf download ggml-org/gemma-3-1b-it-GGUF --local-dir ./models
```

## Build

```bash
git submodule update --init --recursive
cmake --preset=ci-ninja-debug
cmake --build ./build
```

Build only the executable with:

```bash
cmake --build ./build --target octo
```

## Run

Start an interactive conversation:

```bash
./build/octo
```

Run a one-shot request:

```bash
./build/octo ask "Who was John Kennedy?"
```

The current runtime knobs are intentionally not exposed as CLI flags. Model
path, generation budget, and GPU-layer defaults remain internal while the
configuration design is developed.

## Test

```bash
cmake --build ./build --target octopus_tests
ctest -C Debug --test-dir build --output-on-failure
```

If `clang-format` is installed when CMake configures the project:

```bash
cmake --build ./build --target format-check
```

Local AMD HIP and Vulkan development presets are available in
`CMakePresets.json`. They are build-time configurations, not user-facing
runtime backend selection.
