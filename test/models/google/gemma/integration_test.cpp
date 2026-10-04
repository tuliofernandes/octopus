#include "models/google/gemma/integration.hpp"

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

TEST_CASE("Gemma integration remains immovable while borrowing its engine",
          "[gemma]") {
  STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<
                       octopus::models::google::gemma::GemmaIntegration>);
  STATIC_REQUIRE_FALSE(std::is_move_constructible_v<
                       octopus::models::google::gemma::GemmaIntegration>);
}

TEST_CASE("Gemma integration declares current text-only capabilities",
          "[gemma]") {
  FakeTemplateEngine engine(
      octopus::llm::TemplateRenderResult::success("compiled"));
  octopus::models::google::gemma::GemmaIntegration gemma(engine);

  const auto& capabilities = gemma.modelIntegration().capabilities();
  CHECK_FALSE(capabilities.tools);
  CHECK_FALSE(capabilities.structured_output);
  CHECK_FALSE(capabilities.reasoning);
}

TEST_CASE("Gemma compiler prefers the injected metadata template engine",
          "[gemma]") {
  FakeTemplateEngine engine(
      octopus::llm::TemplateRenderResult::success("metadata-rendered prompt"));
  octopus::models::google::gemma::GemmaIntegration gemma(engine);

  const auto compiled =
      gemma.modelIntegration().compiler().compile(policyConversation());

  REQUIRE(compiled.hasValue());
  CHECK(compiled.value().text == "metadata-rendered prompt");
  CHECK(compiled.value().stop_strings ==
        std::vector<std::string>{"<end_of_turn>"});
  CHECK(engine.calls == 1);
  CHECK(engine.last_request.add_generation_prompt);
  CHECK(engine.last_request.reasoning ==
        octopus::llm::TemplateReasoningPolicy::ModelDefault);
  REQUIRE(engine.last_request.messages.size() == 1);
  CHECK(engine.last_request.messages[0].role == "user");
  CHECK(engine.last_request.messages[0].content ==
        "Octopus operating instructions:\n"
        "[system]\n"
        "System policy stays internal.\n"
        "[developer]\n"
        "Developer guidance stays internal.\n"
        "\n"
        "User request:\n"
        "Say hello in one short sentence.");
}

TEST_CASE("Gemma compiler preserves manual prompt fallback", "[gemma]") {
  FakeTemplateEngine engine(
      octopus::llm::TemplateRenderResult::failure("template unavailable"));
  octopus::models::google::gemma::GemmaIntegration gemma(engine);

  const auto compiled =
      gemma.modelIntegration().compiler().compile(policyConversation());

  REQUIRE(compiled.hasValue());
  CHECK(compiled.value().text ==
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
  CHECK(compiled.value().stop_strings ==
        std::vector<std::string>{"<end_of_turn>"});
}

TEST_CASE("Gemma compiler preserves ordered conversation turns", "[gemma]") {
  FakeTemplateEngine engine(
      octopus::llm::TemplateRenderResult::failure("template unavailable"));
  octopus::models::google::gemma::GemmaIntegration gemma(engine);
  octopus::llm::Conversation conversation;
  conversation.messages.push_back({octopus::llm::Role::User, "First question"});
  conversation.messages.push_back(
      {octopus::llm::Role::Assistant, "First answer"});
  conversation.messages.push_back(
      {octopus::llm::Role::User, "Second question"});

  const auto compiled =
      gemma.modelIntegration().compiler().compile(conversation);

  REQUIRE(compiled.hasValue());
  CHECK(compiled.value().text ==
        "<start_of_turn>user\nFirst question<end_of_turn>\n"
        "<start_of_turn>model\nFirst answer<end_of_turn>\n"
        "<start_of_turn>user\nSecond question<end_of_turn>\n"
        "<start_of_turn>model\n");
}

TEST_CASE("Gemma template messages use backend-neutral chat roles", "[gemma]") {
  FakeTemplateEngine engine(
      octopus::llm::TemplateRenderResult::success("metadata-rendered prompt"));
  octopus::models::google::gemma::GemmaIntegration gemma(engine);
  octopus::llm::Conversation conversation;
  conversation.messages.push_back({octopus::llm::Role::User, "First question"});
  conversation.messages.push_back(
      {octopus::llm::Role::Assistant, "First answer"});

  const auto compiled =
      gemma.modelIntegration().compiler().compile(conversation);

  REQUIRE(compiled.hasValue());
  REQUIRE(engine.last_request.messages.size() == 2);
  CHECK(engine.last_request.messages[0].role == "user");
  CHECK(engine.last_request.messages[0].content == "First question");
  CHECK(engine.last_request.messages[1].role == "assistant");
  CHECK(engine.last_request.messages[1].content == "First answer");
}

TEST_CASE("Gemma parser passes through current assistant text", "[gemma]") {
  FakeTemplateEngine engine(
      octopus::llm::TemplateRenderResult::success("compiled"));
  octopus::models::google::gemma::GemmaIntegration gemma(engine);
  octopus::llm::RawCompletion raw;
  raw.text = "assistant text";

  const auto parsed = gemma.modelIntegration().parser().parse(raw);

  REQUIRE(parsed.hasValue());
  CHECK(parsed.value().text == "assistant text");
}

TEST_CASE("Gemma satisfies the reusable model integration contract",
          "[gemma][contract]") {
  FakeTemplateEngine engine(
      octopus::llm::TemplateRenderResult::success("contract question prompt"));
  octopus::models::google::gemma::GemmaIntegration gemma(engine);

  octopus::test::checkModelIntegrationContract(gemma.modelIntegration());
}
