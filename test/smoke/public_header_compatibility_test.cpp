#include "octopus/ask.hpp"
#include "octopus/chat.hpp"
#include "octopus/cli.hpp"
#include "octopus/completion.hpp"
#include "octopus/input_editor.hpp"
#include "octopus/llama_cpp_backend.hpp"
#include "octopus/llm.hpp"
#include "octopus/prompt.hpp"
#include "octopus/terminal_input.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

TEST_CASE("public Octopus headers remain includable", "[headers]") {
  using StopStrings = std::vector<std::string>;

  STATIC_REQUIRE(std::is_default_constructible_v<octopus::CliOptions>);
  STATIC_REQUIRE(std::is_default_constructible_v<octopus::CompletionRequest>);
  STATIC_REQUIRE(std::is_move_constructible_v<octopus::LlamaCppBackend>);
  STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<octopus::LlamaCppBackend>);
  STATIC_REQUIRE(std::is_constructible_v<octopus::StopDetector, StopStrings>);
}
