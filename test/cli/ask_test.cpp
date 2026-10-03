#include "octopus/cli/ask.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <utility>

namespace {

class RecordingCompiler final : public octopus::llm::PromptCompiler {
 public:
  octopus::llm::CompileResult compile(
      const octopus::llm::Conversation& conversation) const override {
    last_conversation = conversation;
    ++calls;
    return octopus::llm::CompileResult::success({"compiled", {"<stop>"}});
  }

  mutable int calls = 0;
  mutable octopus::llm::Conversation last_conversation;
};

class PassThroughParser final : public octopus::llm::AssistantResponseParser {
 public:
  octopus::llm::ParseResult parse(
      const octopus::llm::RawCompletion& completion) const override {
    return octopus::llm::ParseResult::success({completion.text});
  }
};

class FakeBackend final : public octopus::llm::InferenceBackend {
 public:
  explicit FakeBackend(octopus::llm::InferenceResult result)
      : result_(std::move(result)) {}

  octopus::llm::InferenceResult generate(
      const octopus::llm::CompiledPrompt& prompt,
      const octopus::llm::GenerationOptions& generation,
      const octopus::llm::CancellationToken* cancellation,
      octopus::llm::CompletionSink* sink) override {
    last_prompt = prompt;
    last_generation = generation;
    last_cancellation = cancellation;
    last_sink = sink;
    ++calls;
    return result_;
  }

  int calls = 0;
  octopus::llm::CompiledPrompt last_prompt;
  octopus::llm::GenerationOptions last_generation;
  const octopus::llm::CancellationToken* last_cancellation = nullptr;
  octopus::llm::CompletionSink* last_sink = nullptr;

 private:
  octopus::llm::InferenceResult result_;
};

class RuntimeHarness final {
 public:
  explicit RuntimeHarness(octopus::llm::InferenceResult completion)
      : integration_({}, compiler, parser),
        backend(std::move(completion)),
        runtime(integration_, backend) {}

  RecordingCompiler compiler;
  PassThroughParser parser;

 private:
  octopus::llm::ModelIntegration integration_;

 public:
  FakeBackend backend;
  octopus::llm::Runtime runtime;
};

}  // namespace

TEST_CASE("ask options build a model-neutral completion request", "[ask]") {
  octopus::CliOptions options;
  options.mode = octopus::CliMode::Ask;
  options.prompt = "Who was John Kennedy?";
  options.n_predict = 128;
  options.quiet = true;

  const auto request = octopus::makeAskRequest(options);

  REQUIRE(request.conversation.messages.size() == 3);
  CHECK(request.conversation.messages[0].role == octopus::llm::Role::System);
  CHECK_FALSE(request.conversation.messages[0].content.empty());
  CHECK(request.conversation.messages[1].role == octopus::llm::Role::Developer);
  CHECK_FALSE(request.conversation.messages[1].content.empty());
  CHECK(request.conversation.messages[2].role == octopus::llm::Role::User);
  CHECK(request.conversation.messages[2].content == "Who was John Kennedy?");
  CHECK(request.generation.max_tokens == 128);
  CHECK(request.generation.sampler_profile ==
        octopus::llm::SamplerProfile::Deterministic);
  CHECK(request.generation.repeat_last_n == 64);
  CHECK(request.generation.repeat_penalty == 1.05F);
  CHECK(request.generation.frequency_penalty == 0.0F);
  CHECK(request.generation.presence_penalty == 0.0F);
  CHECK(octopus::llm::repeatPenaltyEnabled(request.generation));
}

TEST_CASE("generation options validate repeat penalty policy", "[ask]") {
  octopus::llm::GenerationOptions options;
  CHECK(octopus::llm::validateGenerationOptions(options).ok);

  options.repeat_penalty = 1.05F;
  auto validation = octopus::llm::validateGenerationOptions(options);
  CHECK_FALSE(validation.ok);
  CHECK(validation.error.find("repeat_last_n") != std::string::npos);

  options.repeat_last_n = 64;
  CHECK(octopus::llm::validateGenerationOptions(options).ok);

  options.repeat_penalty = 0.95F;
  validation = octopus::llm::validateGenerationOptions(options);
  CHECK_FALSE(validation.ok);
  CHECK(validation.error.find("repeat_penalty") != std::string::npos);

  options.repeat_penalty = 1.05F;
  options.frequency_penalty = -0.1F;
  validation = octopus::llm::validateGenerationOptions(options);
  CHECK_FALSE(validation.ok);
  CHECK(validation.error.find("frequency_penalty") != std::string::npos);

  options.frequency_penalty = 0.0F;
  options.presence_penalty = -0.1F;
  validation = octopus::llm::validateGenerationOptions(options);
  CHECK_FALSE(validation.ok);
  CHECK(validation.error.find("presence_penalty") != std::string::npos);
}

TEST_CASE("one-shot ask prints only successful completion text", "[ask]") {
  RuntimeHarness harness(octopus::llm::InferenceResult::success(
      {"Hello from the fake backend.",
       octopus::llm::FinishReason::EndOfGeneration, 6}));
  octopus::CliOptions options;
  options.mode = octopus::CliMode::Ask;
  options.prompt = "Say hello";

  std::ostringstream out;
  std::ostringstream err;
  const auto result =
      octopus::runOneShotAsk(options, harness.runtime, out, err);

  CHECK(result.exit_code == 0);
  CHECK(harness.compiler.calls == 1);
  CHECK(harness.backend.calls == 1);
  CHECK(harness.compiler.last_conversation.messages.back().content ==
        "Say hello");
  CHECK(harness.backend.last_prompt.text == "compiled");
  CHECK(harness.backend.last_generation.repeat_last_n == 64);
  CHECK(out.str() == "Hello from the fake backend.\n");
  CHECK(err.str().empty());
}

TEST_CASE("one-shot ask treats loop detection as a successful completion",
          "[ask]") {
  RuntimeHarness harness(octopus::llm::InferenceResult::success(
      {"Useful prefix.", octopus::llm::FinishReason::LoopDetected, 12}));
  octopus::CliOptions options;
  options.mode = octopus::CliMode::Ask;
  options.prompt = "Say hello";

  std::ostringstream out;
  std::ostringstream err;
  const auto result =
      octopus::runOneShotAsk(options, harness.runtime, out, err);

  CHECK(result.exit_code == 0);
  CHECK(harness.backend.calls == 1);
  CHECK(out.str() == "Useful prefix.\n");
  CHECK(err.str().empty());
}

TEST_CASE("one-shot ask reports backend errors on stderr", "[ask]") {
  RuntimeHarness harness(
      octopus::llm::InferenceResult::failure("model failed"));
  octopus::CliOptions options;
  options.mode = octopus::CliMode::Ask;
  options.prompt = "Say hello";

  std::ostringstream out;
  std::ostringstream err;
  const auto result =
      octopus::runOneShotAsk(options, harness.runtime, out, err);

  CHECK(result.exit_code == 1);
  CHECK(out.str().empty());
  CHECK(err.str().find("model failed") != std::string::npos);
}
