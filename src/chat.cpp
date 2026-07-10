#include "octopus/chat.hpp"

#include "octopus/ask.hpp"
#include "octopus/terminal_input.hpp"

#include <csignal>
#include <istream>
#include <ostream>
#include <string>

namespace octopus {
namespace {

constexpr const char* kAssistantPrompt = "octopus> ";
constexpr const char* kUserPrompt = "you> ";

volatile std::sig_atomic_t g_generation_cancelled = 0;

void handleGenerationSigint(int) { g_generation_cancelled = 1; }

class SignalCancellationToken final : public CancellationToken {
 public:
  bool isCancellationRequested() const noexcept override {
    return g_generation_cancelled != 0;
  }
};

class ScopedGenerationCancelHandler final {
 public:
  ScopedGenerationCancelHandler()
      : previous_handler_(std::signal(SIGINT, handleGenerationSigint)) {
    g_generation_cancelled = 0;
  }

  ScopedGenerationCancelHandler(const ScopedGenerationCancelHandler&) = delete;
  ScopedGenerationCancelHandler& operator=(
      const ScopedGenerationCancelHandler&) = delete;

  ~ScopedGenerationCancelHandler() { std::signal(SIGINT, previous_handler_); }

  const CancellationToken& token() const noexcept { return token_; }

 private:
  using SignalHandler = void (*)(int);

  SignalHandler previous_handler_;
  SignalCancellationToken token_;
};

bool completionFailed(const CompletionResult& completion) {
  return completion.finish_reason == FinishReason::BackendError;
}

bool completionCancelled(const CompletionResult& completion) {
  return completion.finish_reason == FinishReason::Cancelled;
}

void reportBackendError(const CompletionResult& completion, std::ostream& err) {
  err << (completion.error.empty() ? "LLM backend error" : completion.error)
      << '\n';
}

class ChatOutputSink final : public CompletionSink {
 public:
  explicit ChatOutputSink(std::ostream& out) : out_(out) {}

  void onText(const CompletionChunk& chunk) override {
    if (chunk.text.empty()) {
      return;
    }

    ensureStarted();
    out_ << chunk.text;
    out_.flush();
  }

  bool started() const noexcept { return started_; }

  void finishSuccess() {
    ensureStarted();
    out_ << '\n';
  }

  void finishError() {
    if (started_) {
      out_ << '\n';
    }
  }

  void finishCancelled() {
    if (started_) {
      out_ << '\n';
    }
  }

 private:
  void ensureStarted() {
    if (!started_) {
      out_ << kAssistantPrompt;
      started_ = true;
    }
  }

  std::ostream& out_;
  bool started_ = false;
};

}  // namespace

ChatRunResult runCliChat(const CliOptions& options, LlmBackend& backend,
                         std::istream& in, std::ostream& out,
                         std::ostream& err) {
  ChatRunResult result;
  Conversation conversation;
  bool previous_input_cancelled = false;

  while (true) {
    const auto input = readTerminalInput(in, out, kUserPrompt);
    if (input.status == TerminalReadStatus::EndOfFile) {
      return result;
    }
    if (input.status == TerminalReadStatus::Cancelled) {
      if (previous_input_cancelled) {
        return result;
      }
      previous_input_cancelled = true;
      continue;
    }
    previous_input_cancelled = false;

    if (input.text.empty()) {
      continue;
    }

    conversation.messages.push_back({Role::User, input.text});
    ChatOutputSink sink(out);
    CompletionRequest request = makeConversationRequest(options, conversation);
    ScopedGenerationCancelHandler cancel_handler;
    request.cancellation = &cancel_handler.token();
    result.last_completion = backend.completeStreaming(request, sink);

    if (completionFailed(result.last_completion)) {
      result.exit_code = 1;
      sink.finishError();
      reportBackendError(result.last_completion, err);
      return result;
    }
    if (completionCancelled(result.last_completion)) {
      sink.finishCancelled();
      conversation.messages.pop_back();
      continue;
    }

    sink.finishSuccess();
    conversation.messages.push_back(
        {Role::Assistant, result.last_completion.text});
  }
}

}  // namespace octopus
