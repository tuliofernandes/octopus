#include "octopus/prompt.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("Gemma instruction profile renders current prompt contract",
          "[prompt]") {
  octopus::Conversation conversation;
  conversation.messages.push_back(
      {octopus::Role::System, "System policy stays internal."});
  conversation.messages.push_back(
      {octopus::Role::Developer, "Developer guidance stays internal."});
  conversation.messages.push_back(
      {octopus::Role::User, "Say hello in one short sentence."});

  const auto rendered = octopus::render_prompt(
      conversation, octopus::ModelProfile::gemma_instruction());

  CHECK(rendered.text ==
        "<start_of_turn>user\n"
        "Octopus operating instructions:\n"
        "[system]\n"
        "System policy stays internal.\n"
        "[developer]\n"
        "Developer guidance stays internal.\n"
        "\n"
        "User request:\n"
        "Say hello in one short sentence.<end_of_turn>\n"
        "<start_of_turn>model\n");
  REQUIRE(rendered.stop_strings.size() == 1);
  CHECK(rendered.stop_strings[0] == "<end_of_turn>");
}

TEST_CASE("Gemma renderer folds policy into one-shot user turn",
          "[prompt]") {
  octopus::Conversation conversation;
  conversation.messages.push_back(
      {octopus::Role::System, "System policy stays internal."});
  conversation.messages.push_back(
      {octopus::Role::Developer, "Developer guidance stays internal."});
  conversation.messages.push_back(
      {octopus::Role::User, "Say hello in one short sentence."});

  const auto rendered = octopus::render_gemma_prompt(conversation);

  CHECK(rendered.text.find("<start_of_turn>user\n") == 0);
  CHECK(rendered.text.find("Octopus operating instructions:\n") !=
        std::string::npos);
  CHECK(rendered.text.find("[system]\nSystem policy stays internal.\n") !=
        std::string::npos);
  CHECK(rendered.text.find("[developer]\nDeveloper guidance stays internal.\n") !=
        std::string::npos);
  CHECK(rendered.text.find("User request:\nSay hello in one short sentence.") !=
        std::string::npos);
  CHECK(rendered.text.find("<end_of_turn>\n<start_of_turn>model\n") !=
        std::string::npos);
  REQUIRE(rendered.stop_strings.size() == 1);
  CHECK(rendered.stop_strings[0] == "<end_of_turn>");
}

TEST_CASE("Gemma renderer preserves ordered user and assistant turns",
          "[prompt]") {
  octopus::Conversation conversation;
  conversation.messages.push_back({octopus::Role::User, "First question"});
  conversation.messages.push_back({octopus::Role::Assistant, "First answer"});
  conversation.messages.push_back({octopus::Role::User, "Second question"});

  const auto rendered = octopus::render_gemma_prompt(conversation);

  const auto first_user =
      rendered.text.find("<start_of_turn>user\nFirst question<end_of_turn>\n");
  const auto assistant = rendered.text.find(
      "<start_of_turn>model\nFirst answer<end_of_turn>\n");
  const auto second_user =
      rendered.text.find("<start_of_turn>user\nSecond question<end_of_turn>\n");
  const auto assistant_prefix = rendered.text.rfind("<start_of_turn>model\n");

  REQUIRE(first_user != std::string::npos);
  REQUIRE(assistant != std::string::npos);
  REQUIRE(second_user != std::string::npos);
  REQUIRE(assistant_prefix != std::string::npos);
  CHECK(first_user < assistant);
  CHECK(assistant < second_user);
  CHECK(second_user < assistant_prefix);
}
