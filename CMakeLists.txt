cmake_minimum_required(VERSION 3.25)

project(LLamaPlayground
        VERSION 1.0.0
        DESCRIPTION "An playground for experimenting with llama.cpp"
        LANGUAGES CXX)

# Export compile commands for use with clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# find Catch2 - if configured Conan2 will be used as dependency provider
find_package(Catch2 REQUIRED)
find_package(argparse REQUIRED)

include(FetchContent)
FetchContent_Declare(
        llama
        GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
        GIT_TAG master
)
FetchContent_MakeAvailable(llama)

# An example library to be used in the test - this could be a library you are developing
add_executable(LLamaPlayground src/main.cpp)
target_include_directories(LLamaPlayground PUBLIC include)
target_compile_features(LLamaPlayground PUBLIC cxx_std_17)
target_link_libraries(LLamaPlayground PRIVATE llama argparse::argparse)

target_compile_options(LLamaPlayground PRIVATE -Wall -Wextra -Wpedantic -Werror)

# Enable testing
include(CTest)



if(Catch2_FOUND AND BUILD_TESTING)
        add_subdirectory(test)
endif()
