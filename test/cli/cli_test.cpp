#include "octopus/cli/cli.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_CASE("CLI defaults to interactive chat mode without model flags",
          "[cli]") {
  const auto result = octopus::parseCli({"octo"});

  REQUIRE(result.ok);
  CHECK(result.exit_code == 0);
  CHECK(result.options.mode == octopus::CliMode::Interactive);
  CHECK(result.options.model == octopus::CliModel::Gemma);
  CHECK(result.options.prompt.empty());
  CHECK(result.options.n_predict == 512);
  CHECK(result.options.n_gpu_layers == 99);
  CHECK(result.options.quiet);
  CHECK(result.error.empty());
  CHECK(result.help.empty());
}

TEST_CASE("CLI parses one-shot ask prompts with the default model", "[cli]") {
  const auto result =
      octopus::parseCli({"octo", "ask", "Who", "was", "John", "Kennedy?"});

  REQUIRE(result.ok);
  CHECK(result.options.mode == octopus::CliMode::Ask);
  CHECK(result.options.prompt == "Who was John Kennedy?");
  CHECK(result.options.model == octopus::CliModel::Gemma);
  CHECK(result.options.n_predict == 512);
  CHECK(result.options.n_gpu_layers == 99);
  CHECK(result.options.quiet);
}

TEST_CASE("CLI parses explicit model selections before either mode", "[cli]") {
  const auto qwen = octopus::parseCli({"octo", "--model", "qwen35"});
  const auto gemma =
      octopus::parseCli({"octo", "--model", "gemma", "ask", "Say", "hello"});

  REQUIRE(qwen.ok);
  CHECK(qwen.options.mode == octopus::CliMode::Interactive);
  CHECK(qwen.options.model == octopus::CliModel::Qwen35);
  REQUIRE(gemma.ok);
  CHECK(gemma.options.mode == octopus::CliMode::Ask);
  CHECK(gemma.options.model == octopus::CliModel::Gemma);
  CHECK(gemma.options.prompt == "Say hello");
}

TEST_CASE("CLI help documents model values and keeps paths private", "[cli]") {
  const auto result = octopus::parseCli({"octo", "--help"});

  CHECK(result.ok);
  CHECK(result.exit_code == 0);
  CHECK(result.help.find("Usage: octo") != std::string::npos);
  CHECK(result.help.find("pure CLI multi-turn chat") != std::string::npos);
  CHECK(result.help.find("octo ask") != std::string::npos);
  CHECK(result.help.find("coming soon") == std::string::npos);
  CHECK(result.help.find("curses") == std::string::npos);
  CHECK(result.help.find("ncurses") == std::string::npos);
  CHECK(result.help.find("TUI") == std::string::npos);
  CHECK(result.help.find("--model <qwen35|gemma>") != std::string::npos);
  CHECK(result.help.find("default: gemma") != std::string::npos);
  CHECK(result.help.find("octo --model gemma ask") != std::string::npos);
  CHECK(result.help.find(".gguf") == std::string::npos);
  CHECK(result.help.find("--n_predict") == std::string::npos);
  CHECK(result.help.find("--n_gpu_layers") == std::string::npos);
  CHECK(result.error.empty());
}

TEST_CASE("CLI requires ask to include a prompt", "[cli]") {
  const auto result = octopus::parseCli({"octo", "ask"});

  CHECK_FALSE(result.ok);
  CHECK(result.exit_code == 1);
  CHECK(result.help.find("Usage: octo") != std::string::npos);
  CHECK(result.error.find("prompt") != std::string::npos);
}

TEST_CASE("CLI rejects low-level runtime flags", "[cli]") {
  const auto result = octopus::parseCli({"octo", "ask", "hello", "-m"});

  CHECK_FALSE(result.ok);
  CHECK(result.exit_code == 1);
  CHECK(result.error.find("-m") != std::string::npos);
  CHECK_FALSE(result.error.empty());
}

TEST_CASE("CLI rejects malformed model selections", "[cli]") {
  const auto missing = octopus::parseCli({"octo", "--model"});
  const auto invalid = octopus::parseCli({"octo", "--model", "other"});
  const auto duplicate =
      octopus::parseCli({"octo", "--model", "qwen35", "--model", "gemma"});
  const auto equals = octopus::parseCli({"octo", "--model=gemma"});

  CHECK_FALSE(missing.ok);
  CHECK(missing.error.find("requires") != std::string::npos);
  CHECK_FALSE(invalid.ok);
  CHECK(invalid.error.find("qwen35|gemma") != std::string::npos);
  CHECK_FALSE(duplicate.ok);
  CHECK(duplicate.error.find("more than once") != std::string::npos);
  CHECK_FALSE(equals.ok);
  CHECK(equals.error.find("--model=gemma") != std::string::npos);
}

TEST_CASE("CLI requires model selection to precede the mode", "[cli]") {
  const auto result =
      octopus::parseCli({"octo", "ask", "hello", "--model", "gemma"});

  CHECK_FALSE(result.ok);
  CHECK(result.error.find("--model must precede the mode") !=
        std::string::npos);
}
