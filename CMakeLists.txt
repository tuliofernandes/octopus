cmake_minimum_required(VERSION 3.25)

project(octopus
        VERSION 1.0.0
        DESCRIPTION "A conservative C++ CLI agent skeleton based on llama.cpp"
        LANGUAGES CXX)

# Export compile commands for use with clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_program(CLANG_FORMAT_EXE NAMES clang-format)

set(OCTOPUS_FORMAT_SOURCES
        include/octopus/cli/ask.hpp
        include/octopus/cli/chat.hpp
        include/octopus/cli/cli.hpp
        include/octopus/cli/input_editor.hpp
        include/octopus/cli/terminal_input.hpp
        include/octopus/inference/backend/llama_cpp_backend.hpp
        include/octopus/inference/harness/completion.hpp
        include/octopus/inference/harness/llm.hpp
        include/octopus/prompt/prompt.hpp
        src/cli/cli.cpp
        src/cli/input/input_editor.cpp
        src/cli/input/terminal_input.cpp
        src/cli/modes/ask.cpp
        src/cli/modes/chat.cpp
        src/inference/harness/completion.cpp
        src/inference/backend/llama_cpp_backend.cpp
        apps/octo/main.cpp
        src/prompt/prompt.cpp
        test/cli/ask_test.cpp
        test/cli/chat_test.cpp
        test/cli/cli_test.cpp
        test/cli/input_editor_test.cpp
        test/cli/terminal_input_test.cpp
        test/inference/completion_test.cpp
        test/prompt/prompt_test.cpp
        test/smoke/public_header_compatibility_test.cpp
        test/smoke/smoke_test.cpp)

if(CLANG_FORMAT_EXE)
        add_custom_target(format-check
                COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror ${OCTOPUS_FORMAT_SOURCES}
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                COMMENT "Checking Octopus C++ formatting")
endif()

# find Catch2 - if configured Conan2 will be used as dependency provider
find_package(Catch2 REQUIRED)
include(FetchContent)
FetchContent_Declare(
        llama
        GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
        GIT_TAG 834a243664114487f99520370a7a7b00fc7a486f
)
FetchContent_MakeAvailable(llama)

add_library(octopus_cli STATIC src/cli/cli.cpp)
target_include_directories(octopus_cli PUBLIC include)
target_compile_features(octopus_cli PUBLIC cxx_std_17)
target_compile_options(octopus_cli PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(octopus_llm STATIC
        src/cli/modes/ask.cpp
        src/cli/modes/chat.cpp
        src/inference/harness/completion.cpp
        src/cli/input/input_editor.cpp
        src/prompt/prompt.cpp
        src/cli/input/terminal_input.cpp)
target_include_directories(octopus_llm PUBLIC include)
target_compile_features(octopus_llm PUBLIC cxx_std_17)
target_link_libraries(octopus_llm PUBLIC octopus_cli)
target_compile_options(octopus_llm PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(octo apps/octo/main.cpp
        src/inference/backend/llama_cpp_backend.cpp)
target_include_directories(octo PUBLIC include)
target_compile_features(octo PUBLIC cxx_std_17)
target_link_libraries(octo PRIVATE octopus_llm llama)

target_compile_options(octo PRIVATE -Wall -Wextra -Wpedantic -Werror)

# Enable testing
include(CTest)



if(Catch2_FOUND AND BUILD_TESTING)
        add_subdirectory(test)
endif()
