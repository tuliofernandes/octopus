#include "models/alibaba/qwen3_5/integration.hpp"

#include "models/contracts/model_integration_contract.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

class FakeTemplateEngine final : public octopus::llm::ChatTemplateEngine {
 public:
  explicit FakeTemplateEngine(octopus::llm::TemplateRenderResult result)
      : result_(std::move(result)) {}

  octopus::llm::TemplateRenderResult render(
      const octopus::llm::ChatTemplateRequest& request) const override {
    last_request = request;
    ++calls;
    return result_;
  }

  mutable int calls = 0;
  mutable octopus::llm::ChatTemplateRequest last_request;

 private:
  octopus::llm::TemplateRenderResult result_;
};

octopus::llm::Conversation policyConversation() {
  octopus::llm::Conversation conversation;
  conversation.messages.push_back(
      {octopus::llm::Role::System, "System policy stays internal."});
  conversation.messages.push_back(
      {octopus::llm::Role::Developer, "Developer guidance stays internal."});
  conversation.messages.push_back(
      {octopus::llm::Role::User, "Say hello in one short sentence."});
  return conversation;
}

}  // namespace

TEST_CASE("Qwen3.5 integration remains immovable while borrowing its engine",
          "[qwen35]") {
  STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<
                       octopus::models::alibaba::qwen3_5::Qwen35Integration>);
  STATIC_REQUIRE_FALSE(std::is_move_constructible_v<
                       octopus::models::alibaba::qwen3_5::Qwen35Integration>);
}

TEST_CASE("Qwen3.5 integration declares current text-only capabilities",
          "[qwen35]") {
  FakeTemplateEngine engine(
      octopus::llm::TemplateRenderResult::success("compiled"));
  octopus::models::alibaba::qwen3_5::Qwen35Integration qwen(engine);

  const auto& capabilities = qwen.modelIntegration().capabilities();
  CHECK_FALSE(capabilities.tools);
  CHECK_FALSE(capabilities.structured_output);
  CHECK_FALSE(capabilities.reasoning);
}

TEST_CASE("Qwen3.5 folds leading policy and disables thinking", "[qwen35]") {
  FakeTemplateEngine engine(
      octopus::llm::TemplateRenderResult::success("metadata-rendered prompt"));
  octopus::models::alibaba::qwen3_5::Qwen35Integration qwen(engine);

  const auto compiled =
      qwen.modelIntegration().compiler().compile(policyConversation());

  REQUIRE(compiled.hasValue());
  CHECK(compiled.value().text == "metadata-rendered prompt");
  CHECK(compiled.value().stop_strings ==
        std::vector<std::string>{"<|im_end|>"});
  CHECK(engine.calls == 1);
  CHECK(engine.last_request.add_generation_prompt);
  CHECK(engine.last_request.reasoning ==
        octopus::llm::TemplateReasoningPolicy::Disabled);
  REQUIRE(engine.last_request.messages.size() == 2);
  CHECK(engine.last_request.messages[0].role == "system");
  CHECK(engine.last_request.messages[0].content ==
        "Octopus operating instructions:\n"
        "[system]\n"
        "System policy stays internal.\n"
        "[developer]\n"
        "Developer guidance stays internal.");
  CHECK(engine.last_request.messages[1].role == "user");
  CHECK(engine.last_request.messages[1].content ==
        "Say hello in one short sentence.");
}

TEST_CASE("Qwen3.5 preserves user and assistant history", "[qwen35]") {
  FakeTemplateEngine engine(
      octopus::llm::TemplateRenderResult::success("compiled"));
  octopus::models::alibaba::qwen3_5::Qwen35Integration qwen(engine);
  octopus::llm::Conversation conversation;
  conversation.messages.push_back({octopus::llm::Role::User, "First question"});
  conversation.messages.push_back(
      {octopus::llm::Role::Assistant, "First answer"});
  conversation.messages.push_back(
      {octopus::llm::Role::User, "Second question"});

  const auto compiled =
      qwen.modelIntegration().compiler().compile(conversation);

  REQUIRE(compiled.hasValue());
  REQUIRE(engine.last_request.messages.size() == 3);
  CHECK(engine.last_request.messages[0].role == "user");
  CHECK(engine.last_request.messages[0].content == "First question");
  CHECK(engine.last_request.messages[1].role == "assistant");
  CHECK(engine.last_request.messages[1].content == "First answer");
  CHECK(engine.last_request.messages[2].role == "user");
  CHECK(engine.last_request.messages[2].content == "Second question");
}

TEST_CASE("Qwen3.5 propagates template failures without fallback", "[qwen35]") {
  FakeTemplateEngine engine(
      octopus::llm::TemplateRenderResult::failure("template failed"));
  octopus::models::alibaba::qwen3_5::Qwen35Integration qwen(engine);

  const auto compiled =
      qwen.modelIntegration().compiler().compile(policyConversation());

  CHECK_FALSE(compiled.hasValue());
  CHECK(compiled.error().find("template failed") != std::string::npos);
  CHECK(engine.calls == 1);
}

TEST_CASE("Qwen3.5 rejects conversations without a user query", "[qwen35]") {
  FakeTemplateEngine engine(
      octopus::llm::TemplateRenderResult::success("compiled"));
  octopus::models::alibaba::qwen3_5::Qwen35Integration qwen(engine);
  octopus::llm::Conversation empty;
  octopus::llm::Conversation policy_only;
  policy_only.messages.push_back(
      {octopus::llm::Role::System, "Policy without a query"});

  const auto empty_result = qwen.modelIntegration().compiler().compile(empty);
  const auto policy_result =
      qwen.modelIntegration().compiler().compile(policy_only);

  CHECK_FALSE(empty_result.hasValue());
  CHECK(empty_result.error().find("user") != std::string::npos);
  CHECK_FALSE(policy_result.hasValue());
  CHECK(policy_result.error().find("user") != std::string::npos);
  CHECK(engine.calls == 0);
}

TEST_CASE("Qwen3.5 rejects late policy messages before rendering", "[qwen35]") {
  FakeTemplateEngine engine(
      octopus::llm::TemplateRenderResult::success("compiled"));
  octopus::models::alibaba::qwen3_5::Qwen35Integration qwen(engine);
  octopus::llm::Conversation conversation;
  conversation.messages.push_back({octopus::llm::Role::User, "Question"});
  conversation.messages.push_back(
      {octopus::llm::Role::Developer, "Late policy"});

  const auto compiled =
      qwen.modelIntegration().compiler().compile(conversation);

  CHECK_FALSE(compiled.hasValue());
  CHECK(compiled.error().find("leading") != std::string::npos);
  CHECK(engine.calls == 0);
}

TEST_CASE("Qwen3.5 parser passes through and owns assistant text", "[qwen35]") {
  FakeTemplateEngine engine(
      octopus::llm::TemplateRenderResult::success("compiled"));
  octopus::models::alibaba::qwen3_5::Qwen35Integration qwen(engine);
  octopus::llm::RawCompletion raw;
  raw.text = "assistant text";

  const auto parsed = qwen.modelIntegration().parser().parse(raw);
  raw.text = "mutated";

  REQUIRE(parsed.hasValue());
  CHECK(parsed.value().text == "assistant text");
}

TEST_CASE("Qwen3.5 satisfies the reusable model integration contract",
          "[qwen35][contract]") {
  FakeTemplateEngine engine(
      octopus::llm::TemplateRenderResult::success("contract question prompt"));
  octopus::models::alibaba::qwen3_5::Qwen35Integration qwen(engine);

  octopus::test::checkModelIntegrationContract(qwen.modelIntegration());
}
