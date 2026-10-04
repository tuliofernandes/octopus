cmake_minimum_required(VERSION 3.25)

include(Catch)

add_executable(octopus_tests)
target_sources(octopus_tests PRIVATE
        cli/ask_test.cpp
        cli/chat_test.cpp
        cli/cli_test.cpp
        cli/input_editor_test.cpp
        cli/terminal_input_test.cpp
        inference/completion_test.cpp
        llm/runtime_test.cpp
        models/alibaba/qwen3_5/integration_test.cpp
        models/google/gemma/integration_test.cpp
        smoke/public_header_compatibility_test.cpp
        smoke/smoke_test.cpp)
target_include_directories(octopus_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(octopus_tests PRIVATE
        Catch2::Catch2WithMain
        octopus_cli_modes
        octopus_model_alibaba_qwen35
        octopus_model_google_gemma)
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
        octopus_model_google_gemma
        octopus_inference_llama_cpp)
target_compile_features(octopus_model_completion_probe PRIVATE cxx_std_17)
target_compile_options(octopus_model_completion_probe PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
