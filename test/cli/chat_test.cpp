#include "octopus/cli/chat.hpp"

#include <catch2/catch_test_macros.hpp>

#include <csignal>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

class RecordingCompiler final : public octopus::llm::PromptCompiler {
 public:
  octopus::llm::CompileResult compile(
      const octopus::llm::Conversation& conversation) const override {
    conversations.push_back(conversation);
    return octopus::llm::CompileResult::success({"compiled", {}});
  }

  mutable std::vector<octopus::llm::Conversation> conversations;
};

class PassThroughParser final : public octopus::llm::AssistantResponseParser {
 public:
  octopus::llm::ParseResult parse(
      const octopus::llm::RawCompletion& completion) const override {
    return octopus::llm::ParseResult::success({completion.text});
  }
};

class RecordingBackend final : public octopus::llm::InferenceBackend {
 public:
  explicit RecordingBackend(std::vector<octopus::llm::InferenceResult> results)
      : results_(std::move(results)) {}

  octopus::llm::InferenceResult generate(
      const octopus::llm::CompiledPrompt&,
      const octopus::llm::GenerationOptions&,
      const octopus::llm::CancellationToken*,
      octopus::llm::CompletionSink* sink) override {
    if (next_result_ >= results_.size()) {
      return octopus::llm::InferenceResult::failure("unexpected backend call");
    }

    const auto result = results_[next_result_];
    ++next_result_;
    if (sink != nullptr && result.hasValue() &&
        result.value().finish_reason != octopus::llm::FinishReason::Cancelled &&
        !result.value().text.empty()) {
      sink->onText({result.value().text});
    }
    return result;
  }

 private:
  std::vector<octopus::llm::InferenceResult> results_;
  std::size_t next_result_ = 0;
};

class StreamingBackend final : public octopus::llm::InferenceBackend {
 public:
  StreamingBackend(std::vector<std::vector<std::string>> chunks,
                   std::vector<octopus::llm::InferenceResult> results)
      : chunks_(std::move(chunks)), results_(std::move(results)) {}

  octopus::llm::InferenceResult generate(
      const octopus::llm::CompiledPrompt&,
      const octopus::llm::GenerationOptions&,
      const octopus::llm::CancellationToken*,
      octopus::llm::CompletionSink* sink) override {
    if (sink == nullptr) {
      ++blocking_calls;
      return octopus::llm::InferenceResult::failure(
          "streaming sink is required");
    }
    if (next_result_ >= results_.size()) {
      return octopus::llm::InferenceResult::failure("unexpected backend call");
    }

    for (const auto& chunk : chunks_[next_result_]) {
      sink->onText({chunk});
    }

    const auto result = results_[next_result_];
    ++next_result_;
    return result;
  }

  int blocking_calls = 0;

 private:
  std::vector<std::vector<std::string>> chunks_;
  std::vector<octopus::llm::InferenceResult> results_;
  std::size_t next_result_ = 0;
};

class CancellingStreamingBackend final : public octopus::llm::InferenceBackend {
 public:
  octopus::llm::InferenceResult generate(
      const octopus::llm::CompiledPrompt&,
      const octopus::llm::GenerationOptions&,
      const octopus::llm::CancellationToken* cancellation,
      octopus::llm::CompletionSink* sink) override {
    ++calls;
    if (sink != nullptr) {
      sink->onText({"Partial"});
    }
    std::raise(SIGINT);
    cancellation_observed =
        cancellation != nullptr && cancellation->isCancellationRequested();
    return octopus::llm::InferenceResult::success(
        {"Partial", octopus::llm::FinishReason::Cancelled, 0});
  }

  int calls = 0;
  bool cancellation_observed = false;
};

template <typename Backend>
class RuntimeHarness final {
 public:
  template <typename... Args>
  explicit RuntimeHarness(Args&&... args)
      : integration_({}, compiler, parser),
        backend(std::forward<Args>(args)...),
        runtime(integration_, backend) {}

  RecordingCompiler compiler;
  PassThroughParser parser;

 private:
  octopus::llm::ModelIntegration integration_;

 public:
  Backend backend;
  octopus::llm::Runtime runtime;
};

octopus::llm::InferenceResult successfulCompletion(std::string text) {
  return octopus::llm::InferenceResult::success(
      {std::move(text), octopus::llm::FinishReason::EndOfGeneration, 0});
}

}  // namespace

TEST_CASE("CLI chat prompts, sends one user line, and exits cleanly on EOF",
          "[chat]") {
  RuntimeHarness<RecordingBackend> harness(
      std::vector<octopus::llm::InferenceResult>{
          successfulCompletion("Hello from Octopus.")});
  octopus::CliOptions options;
  options.mode = octopus::CliMode::Interactive;
  std::istringstream in("Hello\n");
  std::ostringstream out;
  std::ostringstream err;

  const auto result =
      octopus::runCliChat(options, harness.runtime, in, out, err);

  CHECK(result.exit_code == 0);
  REQUIRE(harness.compiler.conversations.size() == 1);
  REQUIRE(harness.compiler.conversations[0].messages.size() == 3);
  CHECK(harness.compiler.conversations[0].messages[0].role ==
        octopus::llm::Role::System);
  CHECK(harness.compiler.conversations[0].messages[1].role ==
        octopus::llm::Role::Developer);
  CHECK(harness.compiler.conversations[0].messages[2].role ==
        octopus::llm::Role::User);
  CHECK(harness.compiler.conversations[0].messages[2].content == "Hello");
  CHECK(out.str() == "> \nHello from Octopus.\n\n> ");
  CHECK(err.str().empty());
}

TEST_CASE("CLI chat preserves assistant replies in later turn history",
          "[chat]") {
  RuntimeHarness<RecordingBackend> harness(
      std::vector<octopus::llm::InferenceResult>{
          successfulCompletion("You said hello."),
          successfulCompletion("You said: Hello")});
  octopus::CliOptions options;
  options.mode = octopus::CliMode::Interactive;
  std::istringstream in("Hello\nWhat did I say?\n");
  std::ostringstream out;
  std::ostringstream err;

  const auto result =
      octopus::runCliChat(options, harness.runtime, in, out, err);

  CHECK(result.exit_code == 0);
  REQUIRE(harness.compiler.conversations.size() == 2);
  const auto& messages = harness.compiler.conversations[1].messages;
  REQUIRE(messages.size() == 5);
  CHECK(messages[2].role == octopus::llm::Role::User);
  CHECK(messages[2].content == "Hello");
  CHECK(messages[3].role == octopus::llm::Role::Assistant);
  CHECK(messages[3].content == "You said hello.");
  CHECK(messages[4].role == octopus::llm::Role::User);
  CHECK(messages[4].content == "What did I say?");
  CHECK(err.str().empty());
}

TEST_CASE("CLI chat renders streaming chunks without duplicating final text",
          "[chat]") {
  RuntimeHarness<StreamingBackend> harness(
      std::vector<std::vector<std::string>>{{"Hello", " from", " Octopus."}},
      std::vector<octopus::llm::InferenceResult>{
          successfulCompletion("Hello from Octopus.")});
  octopus::CliOptions options;
  options.mode = octopus::CliMode::Interactive;
  std::istringstream in("Hello\n");
  std::ostringstream out;
  std::ostringstream err;

  const auto result =
      octopus::runCliChat(options, harness.runtime, in, out, err);

  CHECK(result.exit_code == 0);
  CHECK(harness.backend.blocking_calls == 0);
  CHECK(out.str() == "> \nHello from Octopus.\n\n> ");
  CHECK(err.str().empty());
}

TEST_CASE("CLI chat ignores blank lines without calling the backend",
          "[chat]") {
  RuntimeHarness<RecordingBackend> harness(
      std::vector<octopus::llm::InferenceResult>{
          successfulCompletion("Only once.")});
  octopus::CliOptions options;
  options.mode = octopus::CliMode::Interactive;
  std::istringstream in("\nHello\n\n");
  std::ostringstream out;
  std::ostringstream err;

  const auto result =
      octopus::runCliChat(options, harness.runtime, in, out, err);

  CHECK(result.exit_code == 0);
  CHECK(harness.compiler.conversations.size() == 1);
  CHECK(out.str() == "> > \nOnly once.\n\n> > ");
  CHECK(err.str().empty());
}

TEST_CASE("CLI chat joins continued lines into one user message", "[chat]") {
  RuntimeHarness<RecordingBackend> harness(
      std::vector<octopus::llm::InferenceResult>{
          successfulCompletion("Joined.")});
  octopus::CliOptions options;
  options.mode = octopus::CliMode::Interactive;
  std::istringstream in("First line\\\nsecond line\n");
  std::ostringstream out;
  std::ostringstream err;

  const auto result =
      octopus::runCliChat(options, harness.runtime, in, out, err);

  CHECK(result.exit_code == 0);
  REQUIRE(harness.compiler.conversations.size() == 1);
  REQUIRE(harness.compiler.conversations[0].messages.size() == 3);
  CHECK(harness.compiler.conversations[0].messages[2].content ==
        "First line\nsecond line");
  CHECK(out.str() == "> \nJoined.\n\n> ");
  CHECK(err.str().empty());
}

TEST_CASE("CLI chat records final streaming result as assistant history",
          "[chat]") {
  RuntimeHarness<StreamingBackend> harness(
      std::vector<std::vector<std::string>>{{"First", " answer."},
                                            {"Second", " answer."}},
      std::vector<octopus::llm::InferenceResult>{
          successfulCompletion("First answer."),
          successfulCompletion("Second answer.")});
  octopus::CliOptions options;
  options.mode = octopus::CliMode::Interactive;
  std::istringstream in("Hello\nAgain\n");
  std::ostringstream out;
  std::ostringstream err;

  const auto result =
      octopus::runCliChat(options, harness.runtime, in, out, err);

  CHECK(result.exit_code == 0);
  REQUIRE(harness.compiler.conversations.size() == 2);
  REQUIRE(harness.compiler.conversations[1].messages.size() == 5);
  CHECK(harness.compiler.conversations[1].messages[3].role ==
        octopus::llm::Role::Assistant);
  CHECK(harness.compiler.conversations[1].messages[3].content ==
        "First answer.");
  CHECK(err.str().empty());
}

TEST_CASE("CLI chat reports backend errors and stops without assistant history",
          "[chat]") {
  RuntimeHarness<RecordingBackend> harness(
      std::vector<octopus::llm::InferenceResult>{
          octopus::llm::InferenceResult::failure("model failed")});
  octopus::CliOptions options;
  options.mode = octopus::CliMode::Interactive;
  std::istringstream in("Hello\n");
  std::ostringstream out;
  std::ostringstream err;

  const auto result =
      octopus::runCliChat(options, harness.runtime, in, out, err);

  CHECK(result.exit_code == 1);
  REQUIRE(harness.compiler.conversations.size() == 1);
  REQUIRE(harness.compiler.conversations[0].messages.size() == 3);
  CHECK(harness.compiler.conversations[0].messages.back().role ==
        octopus::llm::Role::User);
  CHECK(out.str() == "> ");
  CHECK(err.str().find("model failed") != std::string::npos);
}

TEST_CASE("CLI chat ends a partial streamed line before reporting an error",
          "[chat]") {
  RuntimeHarness<StreamingBackend> harness(
      std::vector<std::vector<std::string>>{{"Partial answer"}},
      std::vector<octopus::llm::InferenceResult>{
          octopus::llm::InferenceResult::failure("decode failed")});
  octopus::CliOptions options;
  options.mode = octopus::CliMode::Interactive;
  std::istringstream in("Hello\n");
  std::ostringstream out;
  std::ostringstream err;

  const auto result =
      octopus::runCliChat(options, harness.runtime, in, out, err);

  CHECK(result.exit_code == 1);
  CHECK(out.str() == "> \nPartial answer\n");
  CHECK(err.str().find("decode failed") != std::string::npos);
}

TEST_CASE("CLI chat treats loop-detected output as assistant history",
          "[chat]") {
  RuntimeHarness<RecordingBackend> harness(
      std::vector<octopus::llm::InferenceResult>{
          octopus::llm::InferenceResult::success(
              {"Useful prefix.", octopus::llm::FinishReason::LoopDetected, 0}),
          successfulCompletion("The prefix was useful.")});
  octopus::CliOptions options;
  options.mode = octopus::CliMode::Interactive;
  std::istringstream in("Hello\nWhat was useful?\n");
  std::ostringstream out;
  std::ostringstream err;

  const auto result =
      octopus::runCliChat(options, harness.runtime, in, out, err);

  CHECK(result.exit_code == 0);
  REQUIRE(harness.compiler.conversations.size() == 2);
  REQUIRE(harness.compiler.conversations[1].messages.size() == 5);
  CHECK(harness.compiler.conversations[1].messages[3].role ==
        octopus::llm::Role::Assistant);
  CHECK(harness.compiler.conversations[1].messages[3].content ==
        "Useful prefix.");
  CHECK(out.str().find("\nUseful prefix.\n\n") != std::string::npos);
  CHECK(err.str().empty());
}

TEST_CASE("CLI chat exits cleanly on EOF before a message", "[chat]") {
  RuntimeHarness<RecordingBackend> harness(
      std::vector<octopus::llm::InferenceResult>{
          successfulCompletion("unused")});
  octopus::CliOptions options;
  options.mode = octopus::CliMode::Interactive;
  std::istringstream in("");
  std::ostringstream out;
  std::ostringstream err;

  const auto result =
      octopus::runCliChat(options, harness.runtime, in, out, err);

  CHECK(result.exit_code == 0);
  CHECK(harness.compiler.conversations.empty());
  CHECK(out.str() == "> ");
  CHECK(err.str().empty());
}

TEST_CASE("CLI chat handles cancelled generation without assistant history",
          "[chat]") {
  RuntimeHarness<CancellingStreamingBackend> harness;
  octopus::CliOptions options;
  options.mode = octopus::CliMode::Interactive;
  std::istringstream in("Hello\nAgain\n");
  std::ostringstream out;
  std::ostringstream err;

  const auto result =
      octopus::runCliChat(options, harness.runtime, in, out, err);

  CHECK(result.exit_code == 0);
  CHECK(harness.backend.cancellation_observed);
  REQUIRE(harness.compiler.conversations.size() == 2);
  REQUIRE(harness.compiler.conversations[1].messages.size() == 3);
  CHECK(harness.compiler.conversations[1].messages[2].role ==
        octopus::llm::Role::User);
  CHECK(harness.compiler.conversations[1].messages[2].content == "Again");
  CHECK(out.str() == "> \nPartial\n\n> \nPartial\n\n> ");
  CHECK(err.str().empty());
}
