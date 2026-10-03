#pragma once

#include "octopus/llm/contracts.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace octopus::test {

inline void checkModelIntegrationContract(
    const llm::ModelIntegration& integration) {
  llm::Conversation conversation;
  conversation.messages.push_back({llm::Role::User, "contract question"});

  const auto compiled = integration.compiler().compile(conversation);
  REQUIRE(compiled.hasValue());
  CHECK_FALSE(compiled.value().text.empty());

  conversation.messages[0].content = "mutated after compilation";
  CHECK(compiled.value().text.find("contract question") != std::string::npos);

  llm::RawCompletion raw;
  raw.text = "contract answer";
  const auto parsed = integration.parser().parse(raw);
  REQUIRE(parsed.hasValue());

  raw.text = "mutated after parsing";
  CHECK(parsed.value().text == "contract answer");

  static_cast<void>(integration.capabilities());
}

}  // namespace octopus::test
