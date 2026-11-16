cmake_minimum_required(VERSION 3.25)

project(octopus
        VERSION 1.0.0
        DESCRIPTION "A local-first C++ CLI AI agent based on llama.cpp"
        LANGUAGES CXX)

# Export compile commands for use with clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_program(CLANG_FORMAT_EXE NAMES clang-format)

set(OCTOPUS_FORMAT_SOURCES
        include/octopus/chat/runtime.hpp
        src/chat/runtime.cpp
        test/chat/runtime_test.cpp
        include/octopus/core/cancellation.hpp
        include/octopus/tool/tool.hpp
        src/tool/schema.hpp
        src/tool/schema.cpp
        test/tool/schema_test.cpp
        src/tool/tool.cpp
        src/tool/manifest.cpp
        src/tool/loader.cpp
        include/octopus/tool/module.hpp
        tools/misc/hello_world/main.cpp
        tests/tools/misc/hello_world/hello_world_test.cpp
        include/octopus/tool/resources.hpp
        src/tool/resources.cpp
        apps/octo/chat_session.hpp
        apps/octo/chat_session.cpp
        test/tool/tool_test.cpp
        include/octopus/cli/ask.hpp
        include/octopus/cli/chat.hpp
        include/octopus/cli/cli.hpp
        include/octopus/cli/input_editor.hpp
        include/octopus/cli/terminal_input.hpp
        include/octopus/cli/terminal_renderer.hpp
        include/octopus/inference/backend/llama_cpp_backend.hpp
        include/octopus/llm/completion.hpp
        include/octopus/llm/contracts.hpp
        include/octopus/llm/runtime.hpp
        include/octopus/llm/types.hpp
        src/cli/cli.cpp
        src/cli/input/input_editor.cpp
        src/cli/input/terminal_input.cpp
        src/cli/terminal/markdown.cpp
        src/cli/terminal/terminal_renderer.cpp
        src/cli/terminal/transcript.cpp
        src/cli/terminal/text.cpp
        src/cli/modes/ask.cpp
        src/cli/modes/chat.cpp
        src/inference/backend/llama_cpp_backend.cpp
        src/llm/completion.cpp
        src/llm/runtime.cpp
        src/models/alibaba/qwen3_5/integration.cpp
        src/models/alibaba/qwen3_5/integration.hpp
        apps/octo/main.cpp
        test/cli/ask_test.cpp
        test/cli/chat_test.cpp
        test/cli/cli_test.cpp
        test/cli/input_editor_test.cpp
        test/cli/terminal_input_test.cpp
        test/cli/terminal_renderer_test.cpp
        test/inference/completion_test.cpp
        test/integration/model_completion_probe.cpp
        test/integration/model_template_probe.cpp
        test/integration/tool_call_probe.cpp
        test/llm/runtime_test.cpp
        test/models/contracts/model_integration_contract.hpp
        test/models/alibaba/qwen3_5/integration_test.cpp
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
find_package(md4c REQUIRED CONFIG)
find_package(nlohmann_json REQUIRED CONFIG)
find_package(nlohmann_json_schema_validator REQUIRED CONFIG)
find_package(Threads REQUIRED)
include(FetchContent)
FetchContent_Declare(
        llama
        GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
        GIT_TAG 834a243664114487f99520370a7a7b00fc7a486f
)
set(LLAMA_BUILD_COMMON ON CACHE BOOL
        "Build llama.cpp common utilities required by Octopus templates" FORCE)
FetchContent_MakeAvailable(llama)
if(NOT TARGET llama-common)
        message(FATAL_ERROR "Pinned llama.cpp did not provide llama-common")
endif()

add_library(octopus_cli STATIC src/cli/cli.cpp)
target_include_directories(octopus_cli PUBLIC include)
target_compile_features(octopus_cli PUBLIC cxx_std_17)
target_compile_options(octopus_cli PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(octopus_tool STATIC src/tool/schema.cpp src/tool/tool.cpp src/tool/manifest.cpp src/tool/resources.cpp src/tool/loader.cpp)
file(COPY tools/ DESTINATION ${CMAKE_BINARY_DIR}/share/octopus/tools FILES_MATCHING PATTERN "manifest.json")
install(DIRECTORY tools/ DESTINATION share/octopus/tools FILES_MATCHING PATTERN "manifest.json")
add_library(octopus_tool_hello_world MODULE tools/misc/hello_world/main.cpp)
target_include_directories(octopus_tool_hello_world PRIVATE include)
target_compile_features(octopus_tool_hello_world PRIVATE cxx_std_17)
target_compile_options(octopus_tool_hello_world PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(octopus_tool_hello_world PRIVATE nlohmann_json::nlohmann_json)
set_target_properties(octopus_tool_hello_world PROPERTIES PREFIX "" OUTPUT_NAME "tool" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/share/octopus/tools/misc/hello_world")
install(TARGETS octopus_tool_hello_world LIBRARY DESTINATION share/octopus/tools/misc/hello_world)
target_compile_definitions(octopus_tool PRIVATE OCTOPUS_MODULE_SUFFIX="${CMAKE_SHARED_MODULE_SUFFIX}")
target_link_libraries(octopus_tool PUBLIC ${CMAKE_DL_LIBS} PRIVATE nlohmann_json_schema_validator::validator)
target_include_directories(octopus_tool PUBLIC include)
target_compile_features(octopus_tool PUBLIC cxx_std_17)
target_link_libraries(octopus_tool PUBLIC nlohmann_json::nlohmann_json)
target_compile_options(octopus_tool PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(octopus_llm STATIC
        src/llm/completion.cpp
        src/llm/runtime.cpp)
target_include_directories(octopus_llm PUBLIC include)
target_link_libraries(octopus_llm PUBLIC octopus_tool)
target_compile_features(octopus_llm PUBLIC cxx_std_17)
target_compile_options(octopus_llm PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(octopus_chat STATIC src/chat/runtime.cpp)
target_include_directories(octopus_chat PUBLIC include)
target_compile_features(octopus_chat PUBLIC cxx_std_17)
target_link_libraries(octopus_chat PUBLIC octopus_llm octopus_tool)
target_compile_options(octopus_chat PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(octopus_cli_modes STATIC
        src/cli/modes/ask.cpp
        src/cli/modes/chat.cpp
        src/cli/input/input_editor.cpp
        src/cli/input/terminal_input.cpp
        src/cli/terminal/markdown.cpp
        src/cli/terminal/terminal_renderer.cpp
        src/cli/terminal/transcript.cpp
        src/cli/terminal/text.cpp)
target_include_directories(octopus_cli_modes PUBLIC include PRIVATE src)
target_compile_features(octopus_cli_modes PUBLIC cxx_std_17)
target_link_libraries(octopus_cli_modes PUBLIC octopus_cli octopus_llm octopus_chat
        PRIVATE md4c::md4c Threads::Threads)
target_compile_options(octopus_cli_modes PRIVATE -Wall -Wextra -Wpedantic -Werror)


add_library(octopus_model_alibaba_qwen35 STATIC
        src/models/alibaba/qwen3_5/integration.cpp)
target_include_directories(octopus_model_alibaba_qwen35 PUBLIC include src)
target_compile_features(octopus_model_alibaba_qwen35 PUBLIC cxx_std_17)
target_link_libraries(octopus_model_alibaba_qwen35 PUBLIC octopus_llm)
target_compile_options(octopus_model_alibaba_qwen35 PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(octopus_inference_llama_cpp STATIC
        src/inference/backend/llama_cpp_backend.cpp)
target_include_directories(octopus_inference_llama_cpp PUBLIC include)
target_compile_features(octopus_inference_llama_cpp PUBLIC cxx_std_17)
target_link_libraries(octopus_inference_llama_cpp PUBLIC octopus_llm PRIVATE llama-common)
target_compile_options(octopus_inference_llama_cpp PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(octo apps/octo/main.cpp apps/octo/chat_session.cpp)
add_dependencies(octo octopus_tool_hello_world)
target_include_directories(octo PRIVATE include src)
target_compile_features(octo PUBLIC cxx_std_17)
target_link_libraries(octo PRIVATE
        octopus_cli_modes
        octopus_model_alibaba_qwen35
        octopus_inference_llama_cpp)

target_compile_options(octo PRIVATE -Wall -Wextra -Wpedantic -Werror)

# Enable testing
include(CTest)



if(Catch2_FOUND AND BUILD_TESTING)
        add_subdirectory(test)
endif()
