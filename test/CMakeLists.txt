cmake_minimum_required(VERSION 3.25)

include(Catch)

add_executable(octopus_tests)
target_sources(octopus_tests PRIVATE
        ask_test.cpp
        chat_test.cpp
        cli_test.cpp
        completion_test.cpp
        input_editor_test.cpp
        prompt_test.cpp
        smoke_test.cpp
        terminal_input_test.cpp)
target_link_libraries(octopus_tests PRIVATE Catch2::Catch2WithMain octopus_llm)
target_compile_features(octopus_tests PRIVATE cxx_std_17)
target_compile_options(octopus_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)

catch_discover_tests(octopus_tests)
