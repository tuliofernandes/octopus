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

Octopus requires Python 3 with virtual-environment support, CMake 3.25 or
newer, Ninja, Git, and a C++17 toolchain. `clang-format` is optional and only
needed for the `format-check` target.

Install them on a current release of your distribution with one of these
commands:

### Debian and Ubuntu

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build git python3 python3-venv clang-format
```

Ubuntu 22.04 ships a CMake version older than 3.25. Use a newer Ubuntu release
or install a current CMake separately.

### Fedora

```bash
sudo dnf install gcc gcc-c++ cmake ninja-build git python3 clang-tools-extra
```

### Arch Linux

```bash
sudo pacman -S --needed base-devel cmake ninja git python clang
```

### openSUSE

```bash
sudo zypper install gcc gcc-c++ cmake ninja git python3 python3-pip clang-tools
```

### AMD ROCm/HIP build

The `local-amd-hip-debug` preset additionally requires the HIP compiler and
HIP/BLAS development libraries. On Fedora, install the packages used by the
verified local build with:

```bash
sudo dnf install hipcc rocm-hip rocm-hip-devel hipblas-devel rocblas-devel
```

ROCm packaging and supported GPU/distribution combinations differ across
distributions. For non-Fedora systems, install the equivalent HIP SDK and
development libraries from your distribution or AMD before using that preset.

The setup below installs the pinned Conan 2 release in a project-local
`.venv`. Catch2 3.7.1 is then resolved through Conan. llama.cpp is fetched by
CMake at the revision pinned in `CMakeLists.txt`.

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
python3 -m venv .venv
./.venv/bin/python -m pip install -r requirements-build.txt
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
