# LLamaPlayground


This is an example using llama.cpp with C++ to run a local llm in a simple way. 

use the huggingface-cli to download a gguf file like this: 

```bash
hf download ggml-org/gemma-3-1b-it-GGUF --local-dir ./models
```

## Build

To use this template, the submodule [cmake-conan](https://github.com/conan-io/cmake-conan.git) has to be pulled with 
```bash

git submodule update --init --recursive

```

Once this is done the project can be configured and built with

```bash
cmake --preset=ci-ninja-debug
cmake --build ./build
```

Tests can be run using 

```bash
cd build
ctest -C Debug
```

## Prerequisites

* [Conan](https://conan.io) version 2.0 or higher
* [CMake](https://cmake.org) version 3.25 or higher
* [Catch2](https://github.com/catchorg/Catch2) version 3.7 or higher (this will be fetched automatically by conan)
* [Ninja](https://ninja-build.org) (optional, but recommended)

### Recommended VS Code Extensions

* [CMake Tools](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cmake-tools)
* [Test Explorer UI](https://marketplace.visualstudio.com/items?itemName=hbenl.vscode-test-explorer)
  * Alternative [CMake Test Explorer](https://marketplace.visualstudio.com/items?itemName=fredericbonnet.cmake-test-adapter)
* Optional but recommended: [CMake language support](https://marketplace.visualstudio.com/items?itemName=twxs.cmake)
* [Clangd](https://marketplace.visualstudio.com/items?itemName=llvm-vs-code-extensions.vscode-clangd) or [C/C++](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools) for code completion and diagnostics
