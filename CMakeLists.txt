cmake_minimum_required(VERSION 3.25)

project(octopus
        VERSION 1.0.0
        DESCRIPTION "A conservative C++ CLI agent skeleton based on llama.cpp"
        LANGUAGES CXX)

# Export compile commands for use with clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# find Catch2 - if configured Conan2 will be used as dependency provider
find_package(Catch2 REQUIRED)
include(FetchContent)
FetchContent_Declare(
        llama
        GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
        GIT_TAG master
)
FetchContent_MakeAvailable(llama)

add_library(octopus_cli STATIC src/cli.cpp)
target_include_directories(octopus_cli PUBLIC include)
target_compile_features(octopus_cli PUBLIC cxx_std_17)
target_compile_options(octopus_cli PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(octo src/main.cpp)
target_include_directories(octo PUBLIC include)
target_compile_features(octo PUBLIC cxx_std_17)
target_link_libraries(octo PRIVATE octopus_cli llama)

target_compile_options(octo PRIVATE -Wall -Wextra -Wpedantic -Werror)

# Enable testing
include(CTest)



if(Catch2_FOUND AND BUILD_TESTING)
        add_subdirectory(test)
endif()
