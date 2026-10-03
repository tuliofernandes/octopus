#include "octopus/llm/runtime.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

class RecordingCompiler final : public octopus::llm::PromptCompiler {
 public:
  RecordingCompiler(std::vector<std::string>& events,
                    octopus::llm::CompileResult result)
      : events_(events), result_(std::move(result)) {}

  octopus::llm::CompileResult compile(
      const octopus::llm::Conversation& conversation) const override {
    events_.push_back("compile");
    last_conversation = conversation;
    return result_;
  }

  mutable octopus::llm::Conversation last_conversation;

 private:
  std::vector<std::string>& events_;
  octopus::llm::CompileResult result_;
};

class RecordingBackend final : public octopus::llm::InferenceBackend {
 public:
  RecordingBackend(std::vector<std::string>& events,
                   octopus::llm::InferenceResult result)
      : events_(events), result_(std::move(result)) {}

  octopus::llm::InferenceResult generate(
      const octopus::llm::CompiledPrompt& prompt,
      const octopus::llm::GenerationOptions& generation,
      const octopus::llm::CancellationToken* cancellation,
      octopus::llm::CompletionSink* sink) override {
    events_.push_back("generate");
    last_prompt = prompt;
    last_generation = generation;
    last_cancellation = cancellation;
    last_sink = sink;
    return result_;
  }

  octopus::llm::CompiledPrompt last_prompt;
  octopus::llm::GenerationOptions last_generation;
  const octopus::llm::CancellationToken* last_cancellation = nullptr;
  octopus::llm::CompletionSink* last_sink = nullptr;

 private:
  std::vector<std::string>& events_;
  octopus::llm::InferenceResult result_;
};

class RecordingParser final : public octopus::llm::AssistantResponseParser {
 public:
  RecordingParser(std::vector<std::string>& events,
                  octopus::llm::ParseResult result)
      : events_(events), result_(std::move(result)) {}

  octopus::llm::ParseResult parse(
      const octopus::llm::RawCompletion& completion) const override {
    events_.push_back("parse");
    last_completion = completion;
    return result_;
  }

  mutable octopus::llm::RawCompletion last_completion;

 private:
  std::vector<std::string>& events_;
  octopus::llm::ParseResult result_;
};

class FixedCancellationToken final : public octopus::llm::CancellationToken {
 public:
  bool isCancellationRequested() const noexcept override { return true; }
};

octopus::llm::CompileResult compiledPrompt() {
  return octopus::llm::CompileResult::success({"compiled prompt", {"<stop>"}});
}

octopus::llm::InferenceResult rawSuccess() {
  return octopus::llm::InferenceResult::success(
      {"raw answer", octopus::llm::FinishReason::EndOfGeneration, 4});
}

octopus::llm::ParseResult parsedSuccess() {
  return octopus::llm::ParseResult::success({"parsed answer"});
}

}  // namespace

TEST_CASE("result contracts make success and failure mutually exclusive",
          "[llm]") {
  STATIC_REQUIRE_FALSE(std::is_aggregate_v<octopus::llm::CompileResult>);
  STATIC_REQUIRE_FALSE(
      std::is_default_constructible_v<octopus::llm::CompileResult>);

  auto compile_success =
      octopus::llm::CompileResult::success({"compiled prompt", {"<stop>"}});
  REQUIRE(compile_success.hasValue());
  CHECK(compile_success.value().text == "compiled prompt");

  auto compile_failure = octopus::llm::CompileResult::failure("compile failed");
  CHECK_FALSE(compile_failure.hasValue());
  CHECK(compile_failure.error() == "compile failed");

  auto template_failure =
      octopus::llm::TemplateRenderResult::failure("template failed");
  CHECK_FALSE(template_failure.hasValue());
  CHECK(template_failure.error() == "template failed");

  auto inference_failure =
      octopus::llm::InferenceResult::failure("generation failed");
  CHECK_FALSE(inference_failure.hasValue());
  CHECK(inference_failure.error() == "generation failed");

  auto parse_success = octopus::llm::ParseResult::success({"parsed answer"});
  REQUIRE(parse_success.hasValue());
  CHECK(parse_success.value().text == "parsed answer");

  auto completion_success = octopus::llm::CompletionResult::success(
      {{"answer"}, octopus::llm::FinishReason::EndOfGeneration, 3});
  REQUIRE(completion_success.hasValue());
  CHECK(completion_success.value().response.text == "answer");
  CHECK(completion_success.value().generated_tokens == 3);
}

TEST_CASE("borrowed runtime types remain immovable", "[llm]") {
  STATIC_REQUIRE_FALSE(
      std::is_copy_constructible_v<octopus::llm::ModelIntegration>);
  STATIC_REQUIRE_FALSE(
      std::is_move_constructible_v<octopus::llm::ModelIntegration>);
  STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<octopus::llm::Runtime>);
  STATIC_REQUIRE_FALSE(std::is_move_constructible_v<octopus::llm::Runtime>);
}

TEST_CASE("runtime compiles, generates, and parses in order", "[llm]") {
  std::vector<std::string> events;
  RecordingCompiler compiler(events, compiledPrompt());
  RecordingBackend backend(events, rawSuccess());
  RecordingParser parser(events, parsedSuccess());
  const octopus::llm::ModelIntegration integration({}, compiler, parser);
  octopus::llm::Runtime runtime(integration, backend);

  octopus::llm::CompletionRequest request;
  request.conversation.messages.push_back(
      {octopus::llm::Role::User, "question"});
  request.generation.max_tokens = 64;
  const auto result = runtime.complete(request);

  CHECK(events == std::vector<std::string>{"compile", "generate", "parse"});
  REQUIRE(compiler.last_conversation.messages.size() == 1);
  CHECK(compiler.last_conversation.messages[0].content == "question");
  CHECK(backend.last_prompt.text == "compiled prompt");
  CHECK(backend.last_prompt.stop_strings == std::vector<std::string>{"<stop>"});
  CHECK(backend.last_generation.max_tokens == 64);
  CHECK(backend.last_cancellation == nullptr);
  CHECK(backend.last_sink == nullptr);
  CHECK(parser.last_completion.text == "raw answer");
  REQUIRE(result.hasValue());
  CHECK(result.value().response.text == "parsed answer");
  CHECK(result.value().finish_reason ==
        octopus::llm::FinishReason::EndOfGeneration);
  CHECK(result.value().generated_tokens == 4);
}

TEST_CASE("runtime stops after prompt compilation failure", "[llm]") {
  std::vector<std::string> events;
  RecordingCompiler compiler(
      events, octopus::llm::CompileResult::failure("compile failed"));
  RecordingBackend backend(events, rawSuccess());
  RecordingParser parser(events, parsedSuccess());
  const octopus::llm::ModelIntegration integration({}, compiler, parser);
  octopus::llm::Runtime runtime(integration, backend);

  const auto result = runtime.complete({});

  CHECK(events == std::vector<std::string>{"compile"});
  CHECK_FALSE(result.hasValue());
  CHECK(result.error() == "compile failed");
}

TEST_CASE("runtime stops after inference backend failure", "[llm]") {
  std::vector<std::string> events;
  RecordingCompiler compiler(events, compiledPrompt());
  RecordingBackend backend(
      events, octopus::llm::InferenceResult::failure("generate failed"));
  RecordingParser parser(events, parsedSuccess());
  const octopus::llm::ModelIntegration integration({}, compiler, parser);
  octopus::llm::Runtime runtime(integration, backend);

  const auto result = runtime.complete({});

  CHECK(events == std::vector<std::string>{"compile", "generate"});
  CHECK_FALSE(result.hasValue());
  CHECK(result.error() == "generate failed");
}

TEST_CASE("runtime reports assistant response parse failure", "[llm]") {
  std::vector<std::string> events;
  RecordingCompiler compiler(events, compiledPrompt());
  RecordingBackend backend(events, rawSuccess());
  RecordingParser parser(events,
                         octopus::llm::ParseResult::failure("parse failed"));
  const octopus::llm::ModelIntegration integration({}, compiler, parser);
  octopus::llm::Runtime runtime(integration, backend);

  const auto result = runtime.complete({});

  CHECK(events == std::vector<std::string>{"compile", "generate", "parse"});
  CHECK_FALSE(result.hasValue());
  CHECK(result.error() == "parse failed");
}

TEST_CASE("runtime passes cancellation to generation and skips parsing",
          "[llm]") {
  std::vector<std::string> events;
  RecordingCompiler compiler(events, compiledPrompt());
  RecordingBackend backend(
      events, octopus::llm::InferenceResult::success(
                  {"partial", octopus::llm::FinishReason::Cancelled, 2}));
  RecordingParser parser(events, parsedSuccess());
  const octopus::llm::ModelIntegration integration({}, compiler, parser);
  octopus::llm::Runtime runtime(integration, backend);
  FixedCancellationToken cancellation;
  octopus::llm::CompletionRequest request;
  request.cancellation = &cancellation;

  const auto result = runtime.complete(request);

  CHECK(events == std::vector<std::string>{"compile", "generate"});
  CHECK(backend.last_cancellation == &cancellation);
  REQUIRE(result.hasValue());
  CHECK(result.value().response.text == "partial");
  CHECK(result.value().finish_reason == octopus::llm::FinishReason::Cancelled);
}
