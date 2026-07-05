#include "octopus/chat.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

class RecordingBackend final : public octopus::LlmBackend {
public:
  explicit RecordingBackend(std::vector<octopus::CompletionResult> results)
      : results_(std::move(results)) {}

  octopus::CompletionResult
  complete(const octopus::CompletionRequest &request) override {
    requests.push_back(request);
    if (next_result_ >= results_.size()) {
      octopus::CompletionResult completion;
      completion.finish_reason = octopus::FinishReason::BackendError;
      completion.error = "unexpected backend call";
      return completion;
    }

    const auto result = results_[next_result_];
    ++next_result_;
    return result;
  }

  std::vector<octopus::CompletionRequest> requests;

private:
  std::vector<octopus::CompletionResult> results_;
  std::size_t next_result_ = 0;
};

octopus::CompletionResult successful_completion(std::string text) {
  octopus::CompletionResult completion;
  completion.text = std::move(text);
  completion.finish_reason = octopus::FinishReason::EndOfGeneration;
  return completion;
}

} // namespace

TEST_CASE("CLI chat prompts, sends one user line, and exits cleanly on EOF",
          "[chat]") {
  RecordingBackend backend({successful_completion("Hello from Octopus.")});

  octopus::CliOptions options;
  options.mode = octopus::CliMode::Interactive;

  std::istringstream in("Hello\n");
  std::ostringstream out;
  std::ostringstream err;

  const auto result =
      octopus::run_cli_chat(options, backend, in, out, err);

  CHECK(result.exit_code == 0);
  REQUIRE(backend.requests.size() == 1);
  REQUIRE(backend.requests[0].conversation.messages.size() == 3);
  CHECK(backend.requests[0].conversation.messages[0].role ==
        octopus::Role::System);
  CHECK(backend.requests[0].conversation.messages[1].role ==
        octopus::Role::Developer);
  CHECK(backend.requests[0].conversation.messages[2].role ==
        octopus::Role::User);
  CHECK(backend.requests[0].conversation.messages[2].content == "Hello");
  CHECK(out.str() == "you> octopus> Hello from Octopus.\nyou> ");
  CHECK(err.str().empty());
}

TEST_CASE("CLI chat preserves assistant replies in later turn history",
          "[chat]") {
  RecordingBackend backend({successful_completion("You said hello."),
                            successful_completion("You said: Hello")});

  octopus::CliOptions options;
  options.mode = octopus::CliMode::Interactive;

  std::istringstream in("Hello\nWhat did I say?\n");
  std::ostringstream out;
  std::ostringstream err;

  const auto result =
      octopus::run_cli_chat(options, backend, in, out, err);

  CHECK(result.exit_code == 0);
  REQUIRE(backend.requests.size() == 2);
  REQUIRE(backend.requests[1].conversation.messages.size() == 5);
  CHECK(backend.requests[1].conversation.messages[0].role ==
        octopus::Role::System);
  CHECK(backend.requests[1].conversation.messages[1].role ==
        octopus::Role::Developer);
  CHECK(backend.requests[1].conversation.messages[2].role ==
        octopus::Role::User);
  CHECK(backend.requests[1].conversation.messages[2].content == "Hello");
  CHECK(backend.requests[1].conversation.messages[3].role ==
        octopus::Role::Assistant);
  CHECK(backend.requests[1].conversation.messages[3].content ==
        "You said hello.");
  CHECK(backend.requests[1].conversation.messages[4].role ==
        octopus::Role::User);
  CHECK(backend.requests[1].conversation.messages[4].content ==
        "What did I say?");
  CHECK(err.str().empty());
}

TEST_CASE("CLI chat ignores blank lines without calling the backend",
          "[chat]") {
  RecordingBackend backend({successful_completion("Only once.")});

  octopus::CliOptions options;
  options.mode = octopus::CliMode::Interactive;

  std::istringstream in("\nHello\n\n");
  std::ostringstream out;
  std::ostringstream err;

  const auto result =
      octopus::run_cli_chat(options, backend, in, out, err);

  CHECK(result.exit_code == 0);
  CHECK(backend.requests.size() == 1);
  CHECK(out.str() == "you> you> octopus> Only once.\nyou> you> ");
  CHECK(err.str().empty());
}

TEST_CASE("CLI chat reports backend errors and stops without assistant history",
          "[chat]") {
  octopus::CompletionResult error;
  error.finish_reason = octopus::FinishReason::BackendError;
  error.error = "model failed";
  RecordingBackend backend({error});

  octopus::CliOptions options;
  options.mode = octopus::CliMode::Interactive;

  std::istringstream in("Hello\n");
  std::ostringstream out;
  std::ostringstream err;

  const auto result =
      octopus::run_cli_chat(options, backend, in, out, err);

  CHECK(result.exit_code == 1);
  REQUIRE(backend.requests.size() == 1);
  REQUIRE(backend.requests[0].conversation.messages.size() == 3);
  CHECK(backend.requests[0].conversation.messages.back().role ==
        octopus::Role::User);
  CHECK(out.str() == "you> ");
  CHECK(err.str().find("model failed") != std::string::npos);
}

TEST_CASE("CLI chat treats loop-detected output as assistant history",
          "[chat]") {
  octopus::CompletionResult loop_detected;
  loop_detected.text = "Useful prefix.";
  loop_detected.finish_reason = octopus::FinishReason::LoopDetected;
  RecordingBackend backend(
      {loop_detected, successful_completion("The prefix was useful.")});

  octopus::CliOptions options;
  options.mode = octopus::CliMode::Interactive;

  std::istringstream in("Hello\nWhat was useful?\n");
  std::ostringstream out;
  std::ostringstream err;

  const auto result =
      octopus::run_cli_chat(options, backend, in, out, err);

  CHECK(result.exit_code == 0);
  REQUIRE(backend.requests.size() == 2);
  REQUIRE(backend.requests[1].conversation.messages.size() == 5);
  CHECK(backend.requests[1].conversation.messages[3].role ==
        octopus::Role::Assistant);
  CHECK(backend.requests[1].conversation.messages[3].content ==
        "Useful prefix.");
  CHECK(out.str().find("octopus> Useful prefix.\n") != std::string::npos);
  CHECK(err.str().empty());
}
