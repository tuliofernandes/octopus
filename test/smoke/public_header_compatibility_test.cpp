#include "octopus/cli/ask.hpp"
#include "octopus/cli/chat.hpp"
#include "octopus/cli/cli.hpp"
#include "octopus/cli/input_editor.hpp"
#include "octopus/cli/terminal_input.hpp"
#include "octopus/inference/backend/llama_cpp_backend.hpp"
#include "octopus/inference/harness/completion.hpp"
#include "octopus/inference/harness/llm.hpp"
#include "octopus/prompt/prompt.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

TEST_CASE("public Octopus architecture headers remain includable",
          "[headers]") {
  using StopStrings = std::vector<std::string>;

  STATIC_REQUIRE(std::is_default_constructible_v<octopus::CliOptions>);
  STATIC_REQUIRE(std::is_default_constructible_v<octopus::CompletionRequest>);
  STATIC_REQUIRE(std::is_move_constructible_v<octopus::LlamaCppBackend>);
  STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<octopus::LlamaCppBackend>);
  STATIC_REQUIRE(std::is_constructible_v<octopus::StopDetector, StopStrings>);
}
