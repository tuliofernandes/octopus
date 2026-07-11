#include "octopus/cli/ask.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <utility>

namespace {

class FakeBackend final : public octopus::LlmBackend {
 public:
  explicit FakeBackend(octopus::CompletionResult result)
      : result_(std::move(result)) {}

  octopus::CompletionResult complete(
      const octopus::CompletionRequest& request) override {
    last_request = request;
    ++calls;
    return result_;
  }

  int calls = 0;
  octopus::CompletionRequest last_request;

 private:
  octopus::CompletionResult result_;
};

}  // namespace

TEST_CASE("ask options build a message-shaped completion request", "[ask]") {
  octopus::CliOptions options;
  options.mode = octopus::CliMode::Ask;
  options.prompt = "Who was John Kennedy?";
  options.n_predict = 128;
  options.quiet = true;

  const auto request = octopus::makeAskRequest(options);

  REQUIRE(request.conversation.messages.size() == 3);
  CHECK(request.conversation.messages[0].role == octopus::Role::System);
  CHECK_FALSE(request.conversation.messages[0].content.empty());
  CHECK(request.conversation.messages[1].role == octopus::Role::Developer);
  CHECK_FALSE(request.conversation.messages[1].content.empty());
  CHECK(request.conversation.messages[2].role == octopus::Role::User);
  CHECK(request.conversation.messages[2].content == "Who was John Kennedy?");
  CHECK(request.generation.max_tokens == 128);
  CHECK(request.generation.sampler_profile ==
        octopus::SamplerProfile::Deterministic);
  CHECK(request.generation.repeat_last_n == 64);
  CHECK(request.generation.repeat_penalty == 1.05F);
  CHECK(request.generation.frequency_penalty == 0.0F);
  CHECK(request.generation.presence_penalty == 0.0F);
  CHECK(octopus::repeatPenaltyEnabled(request.generation));
  REQUIRE(request.generation.stop_strings.size() == 1);
  CHECK(request.generation.stop_strings[0] == "<end_of_turn>");
}

TEST_CASE("generation options validate repeat penalty policy", "[ask]") {
  octopus::GenerationOptions options;
  CHECK(octopus::validateGenerationOptions(options).ok);

  options.repeat_penalty = 1.05F;
  auto validation = octopus::validateGenerationOptions(options);
  CHECK_FALSE(validation.ok);
  CHECK(validation.error.find("repeat_last_n") != std::string::npos);

  options.repeat_last_n = 64;
  CHECK(octopus::validateGenerationOptions(options).ok);

  options.repeat_penalty = 0.95F;
  validation = octopus::validateGenerationOptions(options);
  CHECK_FALSE(validation.ok);
  CHECK(validation.error.find("repeat_penalty") != std::string::npos);

  options.repeat_penalty = 1.05F;
  options.frequency_penalty = -0.1F;
  validation = octopus::validateGenerationOptions(options);
  CHECK_FALSE(validation.ok);
  CHECK(validation.error.find("frequency_penalty") != std::string::npos);

  options.frequency_penalty = 0.0F;
  options.presence_penalty = -0.1F;
  validation = octopus::validateGenerationOptions(options);
  CHECK_FALSE(validation.ok);
  CHECK(validation.error.find("presence_penalty") != std::string::npos);
}

TEST_CASE("ask request accepts selected model profile policy", "[ask]") {
  octopus::CliOptions options;
  options.mode = octopus::CliMode::Ask;
  options.prompt = "Say hello";

  auto profile = octopus::ModelProfile::gemmaInstruction();
  profile.stop_strings = {"<custom-profile-stop>"};

  const auto request = octopus::makeAskRequest(options, profile);

  CHECK(request.model_profile.prompt_renderer ==
        octopus::PromptRenderer::LlamaChatTemplate);
  CHECK(request.model_profile.fallback_renderer ==
        octopus::PromptFallback::GemmaInstruction);
  REQUIRE(request.generation.stop_strings.size() == 1);
  CHECK(request.generation.stop_strings[0] == "<custom-profile-stop>");
}

TEST_CASE("one-shot ask prints only successful completion text", "[ask]") {
  octopus::CompletionResult completion;
  completion.text = "Hello from the fake backend.";
  completion.finish_reason = octopus::FinishReason::EndOfGeneration;
  completion.generated_tokens = 6;
  FakeBackend backend(completion);

  octopus::CliOptions options;
  options.mode = octopus::CliMode::Ask;
  options.prompt = "Say hello";

  std::ostringstream out;
  std::ostringstream err;
  const auto result = octopus::runOneShotAsk(options, backend, out, err);

  CHECK(result.exit_code == 0);
  CHECK(backend.calls == 1);
  CHECK(backend.last_request.model_profile.prompt_renderer ==
        octopus::PromptRenderer::LlamaChatTemplate);
  CHECK(backend.last_request.model_profile.fallback_renderer ==
        octopus::PromptFallback::GemmaInstruction);
  REQUIRE(backend.last_request.generation.stop_strings.size() == 1);
  CHECK(backend.last_request.generation.stop_strings[0] == "<end_of_turn>");
  CHECK(backend.last_request.conversation.messages.back().content ==
        "Say hello");
  CHECK(out.str() == "Hello from the fake backend.\n");
  CHECK(err.str().empty());
}

TEST_CASE("one-shot ask treats loop detection as a successful completion",
          "[ask]") {
  octopus::CompletionResult completion;
  completion.text = "Useful prefix.";
  completion.finish_reason = octopus::FinishReason::LoopDetected;
  completion.generated_tokens = 12;
  FakeBackend backend(completion);

  octopus::CliOptions options;
  options.mode = octopus::CliMode::Ask;
  options.prompt = "Say hello";

  std::ostringstream out;
  std::ostringstream err;
  const auto result = octopus::runOneShotAsk(options, backend, out, err);

  CHECK(result.exit_code == 0);
  CHECK(backend.calls == 1);
  CHECK(out.str() == "Useful prefix.\n");
  CHECK(err.str().empty());
}

TEST_CASE("one-shot ask reports backend errors on stderr", "[ask]") {
  octopus::CompletionResult completion;
  completion.finish_reason = octopus::FinishReason::BackendError;
  completion.error = "model failed";
  FakeBackend backend(completion);

  octopus::CliOptions options;
  options.mode = octopus::CliMode::Ask;
  options.prompt = "Say hello";

  std::ostringstream out;
  std::ostringstream err;
  const auto result = octopus::runOneShotAsk(options, backend, out, err);

  CHECK(result.exit_code == 1);
  CHECK(out.str().empty());
  CHECK(err.str().find("model failed") != std::string::npos);
}
