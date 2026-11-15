cmake_minimum_required(VERSION 3.25)

include(Catch)

add_executable(octopus_tests)
target_sources(octopus_tests PRIVATE
        cli/ask_test.cpp
        cli/chat_test.cpp
        cli/cli_test.cpp
        cli/input_editor_test.cpp
        cli/terminal_input_test.cpp
        cli/terminal_renderer_test.cpp
        inference/completion_test.cpp
        llm/runtime_test.cpp
        models/alibaba/qwen3_5/integration_test.cpp
        smoke/public_header_compatibility_test.cpp
        smoke/smoke_test.cpp)
target_include_directories(octopus_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(octopus_tests PRIVATE
        Catch2::Catch2WithMain
        octopus_cli_modes
        octopus_model_alibaba_qwen35)
target_compile_features(octopus_tests PRIVATE cxx_std_17)
target_compile_options(octopus_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)

catch_discover_tests(octopus_tests)

# Explicit opt-in probe for real GGUF template rendering. It is intentionally
# excluded from the model-free default build and CTest discovery.
add_executable(octopus_model_template_probe EXCLUDE_FROM_ALL
        integration/model_template_probe.cpp)
target_link_libraries(octopus_model_template_probe PRIVATE
        octopus_inference_llama_cpp)
target_compile_features(octopus_model_template_probe PRIVATE cxx_std_17)
target_compile_options(octopus_model_template_probe PRIVATE
        -Wall -Wextra -Wpedantic -Werror)

# Explicit opt-in probe for real completion behavior and local performance
# gates. It requires a model path and is excluded from model-free CTest.
add_executable(octopus_model_completion_probe EXCLUDE_FROM_ALL
        integration/model_completion_probe.cpp)
target_link_libraries(octopus_model_completion_probe PRIVATE
        octopus_cli_modes
        octopus_model_alibaba_qwen35
        octopus_inference_llama_cpp)
target_compile_features(octopus_model_completion_probe PRIVATE cxx_std_17)
target_compile_options(octopus_model_completion_probe PRIVATE
        -Wall -Wextra -Wpedantic -Werror)

add_executable(octopus_tool_tests tool/tool_test.cpp ../tests/tools/misc/hello_world/hello_world_test.cpp)
add_dependencies(octopus_tool_tests octopus_tool_hello_world)
target_link_libraries(octopus_tool_tests PRIVATE octopus_tool Catch2::Catch2WithMain)
target_compile_features(octopus_tool_tests PRIVATE cxx_std_17)
target_compile_definitions(octopus_tool_tests PRIVATE TOOL_RESOURCE_ROOT="${CMAKE_BINARY_DIR}/share/octopus/tools" TOOL_MODULE_SUFFIX="${CMAKE_SHARED_MODULE_SUFFIX}")
catch_discover_tests(octopus_tool_tests)
target_sources(octopus_tests PRIVATE chat/runtime_test.cpp)
target_link_libraries(octopus_tests PRIVATE octopus_chat)

add_executable(octopus_tool_call_probe EXCLUDE_FROM_ALL integration/tool_call_probe.cpp)
add_dependencies(octopus_tool_call_probe octopus_tool_hello_world)
target_include_directories(octopus_tool_call_probe PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(octopus_tool_call_probe PRIVATE octopus_chat octopus_cli_modes octopus_model_alibaba_qwen35 octopus_inference_llama_cpp)
target_compile_features(octopus_tool_call_probe PRIVATE cxx_std_17)
target_compile_options(octopus_tool_call_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
