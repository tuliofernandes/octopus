# Octopus

Octopus is a local-first conversational AI agent for UNIX/Linux, written in
C++17 and powered by llama.cpp. The current product is a terminal CLI named
`octo` with one-shot and multi-turn conversation modes.

Current capabilities include:

- completed-turn Markdown rendering in interactive terminals, while backend
  generation remains streamed internally;
- a multi-turn conversation loop;
- one-shot `ask` mode;
- `> ` interactive prompts and multiline editing with UTF-8 code-point cursor
  and deletion behavior, terminal-width-aware redraw, paste, Ctrl+C
  cancellation, and Ctrl+D exit;
- a model-neutral runtime contract for prompt compilation, capabilities,
  inference, and assistant-response parsing;
- an Alibaba Qwen3.5 integration that uses the GGUF's embedded Jinja
  template with thinking disabled;
- typed `--model qwen35` selection while loading one model per process;
- testable runtime, model-integration, inference, and CLI boundaries that do
  not require a model in unit tests.

Persistence, runtime backend selection, configuration loading, and a daemon
are not implemented yet.

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
`.venv`. Catch2 3.7.1, nlohmann_json 3.11.3, and the MIT-licensed MD4C 0.5.2 parser are then resolved
through Conan; only MD4C's parser library is used. llama.cpp is fetched by CMake
at the revision pinned in `CMakeLists.txt`.

## Models

Qwen3.5 is the default model, verified on the local AMD HIP configuration.
CPU short requests are operational; the CPU long-context promotion gate exceeded
its time limit. See the [Qwen3.5 notes](src/models/alibaba/qwen3_5/README.md)
for template, capability, and verification details.

Download the exact approved Qwen3.5 artifact with:

```bash
hf download unsloth/Qwen3.5-4B-GGUF Qwen3.5-4B-Q4_K_M.gguf \
  --revision 720bb031aae5488eae5d6a78768e6d826662b2ae \
  --local-dir ./models
echo "00fe7986ff5f6b463e62455821146049db6f9313603938a70800d1fb69ef11a4  models/Qwen3.5-4B-Q4_K_M.gguf" \
  | sha256sum -c -
```

The file is 2,740,937,888 bytes. Its base model license is Apache-2.0. Model
weights remain separately licensed local artifacts and are ignored by Git; do
not package or commit them.

On the verified host, Qwen's short HIP requests used about 2.9 GiB peak RSS and
about 3.0 GiB additional RX 7800 XT VRAM; CPU fallback used about 4.0 GiB peak
RSS and was much slower. Leave headroom beyond these measurements. No projector
is needed for this text-only phase.

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
./build/octo --model qwen35
```

Interactive input uses a `> ` prompt (`... ` on continuation rows). During
generation, Octopus shows an animated transient spinner and removes it before
writing the complete parsed assistant response once. The dormant thinking and
elapsed-time presentation path is retained for future structured thinking but
is not displayed today. Raw generation chunks are not displayed. Exactly one
blank row separates user input and assistant output in both directions.

When terminal color discovery succeeds, each assistant response receives a
subtly contrasting full-width background derived from the terminal's own
background: dark themes become slightly lighter and light themes become
slightly darker. Octopus queries the terminal with OSC 11 and uses `COLORFGBG`
when available; it emits truecolor when `COLORTERM` advertises it and otherwise
chooses a distinct nearby 256-color value. Unsupported terminals simply omit
the response background rather than assuming a fixed color. The final response
supports headings, emphasis/strong text, lists and task items, inline and
fenced code, links, block quotes, line breaks, autolinks, and strikethrough.
Block quotes use vertical bars on each line, including wrapped lines; terminals
without UTF-8 use ASCII bars. Other content uses native
terminal wrapping; there is no response gutter, manual response wrapping,
alternate screen, cursor hiding, or OSC hyperlink. Tables are aligned when they
fit the terminal; wider tables retain their rows and use native wrapping. A
table exceeding 16,384 cells falls back to sanitized raw Markdown. Links are
underlined, and named HTML entities are decoded. Raw HTML remains visible
literal text. The entity table can be verified with
`python3 scripts/generate_named_entities.py --check`; the generator documents
the source and Python version. Cancellation clears the
spinner and does not publish a partial answer.

Enhanced rendering requires TTY stdin and stdout, a non-empty `TERM` other than
`dumb`, and a usable terminal width for transient activity. `TERM=dumb` keeps
the TTY prompt and blank-row transcript spacing but omits activity and ANSI
styling. Pipes and redirects receive sanitized raw Markdown with no prompts,
activity, ANSI styling, or renderer-created line wrapping. A non-empty
`NO_COLOR` removes semantic and response-background color only; cursor updates
and semantic bold/italic/strikethrough remain available. An empty `NO_COLOR`
value does not disable color. Invalid UTF-8, model-provided terminal control
bytes, and delayed terminal color-query replies are filtered before output or
input submission.

Precomposed accented input is supported for insertion, cursor movement, and
deletion. Full grapheme-cluster editing for combining sequences and emoji is a
later capability. Terminal resize while an input/status line is already live
is best-effort; width is queried again for later redraws and status updates.

Run a one-shot request:

```bash
./build/octo ask "Who was John Kennedy?"
./build/octo --model qwen35 ask "Who was John Kennedy?"
```

One-shot `ask` always prints sanitized raw assistant text rather than terminal
Markdown styling, including when stdout is a TTY. This keeps it suitable for
pipes and command substitution.

`--model` must precede the mode. It selects one supported family; it does not
accept a filesystem path. Model paths, generation budget, GPU layers,
quantization, and sampling knobs remain internal while the configuration
design is developed.

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

## Tool foundation

The independent `octopus_tool` library loads JSON manifests from an explicit
resource root, discovers tools in category/name directories and loads their
`tool` shared modules into the Octopus process. Each module exports the
versioned `octopusToolModule` entry point declared in `octopus/tool/module.hpp`;
it supplies the tool’s validator and handler. Modules must use a compatible C++
ABI and nlohmann_json version, and are trusted native code: loading and running
them is not sandboxed. Identifiers use
lowercase letters, digits and underscores and start with a letter. Manifests declare object parameter schemas for the model; each registry entry
must supply a validator that enforces its tool’s arguments. The loader checks
the schema root, not full JSON Schema correctness, so authors must keep the
manifest and validator consistent. `misc.hello_world` accepts only an empty
object. Its implementation and manifest live in `tools/misc/hello_world/`,
with tests in `tests/tools/misc/hello_world/`. CMake builds each module beside
its copied manifest and installs both under `share/octopus/tools`; new modules
need a corresponding build and install target. The executor requires a
security policy; `AllowAllSecurityPolicy` permits every valid invocation and
provides no isolation or permission enforcement. Interactive chat advertises registered tools and executes calls serially,
then returns correlated results to the model. One-shot `ask` does not use tools.
A turn is limited to eight tool rounds. Build resources are copied to `build/share/octopus/tools`;
installation places them under `share/octopus/tools`. The caller supplies the
resource root explicitly. Run `cmake --build build --target octopus_tool_tests`
and `./build/test/octopus_tool_tests` for the model-free subsystem tests.

To verify Qwen tool execution against a model, build the opt-in probe and pass
the model and resource paths explicitly:

```bash
cmake --build build --target octopus_tool_call_probe
./build/test/octopus_tool_call_probe ./models/Qwen3.5-4B-Q4_K_M.gguf ./build/share/octopus/tools
```

The probe verifies two distinct calls, their results in later model prompts and
answers, and a normal conversation turn. Use the corresponding build directory
for an AMD HIP build.
