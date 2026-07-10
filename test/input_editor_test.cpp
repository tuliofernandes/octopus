#include "octopus/input_editor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <sstream>
#include <string>

TEST_CASE("input editor reads one submitted cooked line", "[input-editor]") {
  std::istringstream in("hello\n");
  std::ostringstream out;
  octopus::InputEditor editor(in, out, {"you> ", "...> "});

  const auto input = editor.read();

  CHECK(input.status == octopus::InputEditorStatus::Submitted);
  CHECK(input.text == "hello");
  CHECK(out.str() == "you> ");
}

TEST_CASE("input editor exposes a readText convenience for submitted input",
          "[input-editor]") {
  std::istringstream in("hello\n");
  std::ostringstream out;
  octopus::InputEditor editor(in, out, {"you> ", "...> "});

  const std::optional<std::string> input = editor.readText();

  REQUIRE(input.has_value());
  CHECK(*input == "hello");
}

TEST_CASE("input editor joins cooked continuation lines", "[input-editor]") {
  std::istringstream in("hello\\\nworld\n");
  std::ostringstream out;
  octopus::InputEditor editor(in, out, {"you> ", "...> "});

  const auto input = editor.read();

  CHECK(input.status == octopus::InputEditorStatus::Submitted);
  CHECK(input.text == "hello\nworld");
  CHECK(out.str() == "you> ");
}

TEST_CASE("input editor reports EOF before input", "[input-editor]") {
  std::istringstream in("");
  std::ostringstream out;
  octopus::InputEditor editor(in, out, {"you> ", "...> "});

  const auto input = editor.read();

  CHECK(input.status == octopus::InputEditorStatus::EndOfFile);
  CHECK(input.text.empty());
  CHECK(out.str() == "you> ");
}

TEST_CASE("input editor readText returns empty optional on EOF",
          "[input-editor]") {
  std::istringstream in("");
  std::ostringstream out;
  octopus::InputEditor editor(in, out, {"you> ", "...> "});

  const std::optional<std::string> input = editor.readText();

  CHECK_FALSE(input.has_value());
}
