#include "octopus/cli.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_CASE("CLI defaults to interactive chat mode without model flags", "[cli]") {
  const auto result = octopus::parse_cli({"octo"});

  REQUIRE(result.ok);
  CHECK(result.exit_code == 0);
  CHECK(result.options.mode == octopus::CliMode::Interactive);
  CHECK(result.options.model_path == "./models/gemma-3-1b-it-Q4_K_M.gguf");
  CHECK(result.options.prompt.empty());
  CHECK(result.options.n_predict == 512);
  CHECK(result.options.n_gpu_layers == 99);
  CHECK(result.options.quiet);
  CHECK(result.error.empty());
  CHECK(result.help.empty());
}

TEST_CASE("CLI parses one-shot ask prompts without exposing model knobs",
          "[cli]") {
  const auto result = octopus::parse_cli(
      {"octo", "ask", "Who", "was", "John", "Kennedy?"});

  REQUIRE(result.ok);
  CHECK(result.options.mode == octopus::CliMode::Ask);
  CHECK(result.options.prompt == "Who was John Kennedy?");
  CHECK(result.options.model_path == "./models/gemma-3-1b-it-Q4_K_M.gguf");
  CHECK(result.options.n_predict == 512);
  CHECK(result.options.n_gpu_layers == 99);
  CHECK(result.options.quiet);
}

TEST_CASE("CLI help exposes modes instead of low-level runtime knobs", "[cli]") {
  const auto result = octopus::parse_cli({"octo", "--help"});

  CHECK(result.ok);
  CHECK(result.exit_code == 0);
  CHECK(result.help.find("Usage: octo") != std::string::npos);
  CHECK(result.help.find("pure CLI multi-turn chat") != std::string::npos);
  CHECK(result.help.find("octo ask") != std::string::npos);
  CHECK(result.help.find("coming soon") == std::string::npos);
  CHECK(result.help.find("curses") == std::string::npos);
  CHECK(result.help.find("ncurses") == std::string::npos);
  CHECK(result.help.find("TUI") == std::string::npos);
  CHECK(result.help.find("--model") == std::string::npos);
  CHECK(result.help.find("--n_predict") == std::string::npos);
  CHECK(result.help.find("--n_gpu_layers") == std::string::npos);
  CHECK(result.error.empty());
}

TEST_CASE("CLI requires ask to include a prompt", "[cli]") {
  const auto result = octopus::parse_cli({"octo", "ask"});

  CHECK_FALSE(result.ok);
  CHECK(result.exit_code == 1);
  CHECK(result.help.find("Usage: octo") != std::string::npos);
  CHECK(result.error.find("prompt") != std::string::npos);
}

TEST_CASE("CLI rejects low-level runtime flags", "[cli]") {
  const auto result = octopus::parse_cli({"octo", "ask", "--model", "x.gguf"});

  CHECK_FALSE(result.ok);
  CHECK(result.exit_code == 1);
  CHECK(result.error.find("--model") != std::string::npos);
  CHECK_FALSE(result.error.empty());
}
