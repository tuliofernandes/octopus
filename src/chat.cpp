#include "octopus/chat.hpp"

#include "octopus/ask.hpp"

#include <istream>
#include <ostream>
#include <string>

namespace octopus {
namespace {

constexpr const char* kAssistantPrompt = "octopus> ";

bool completionFailed(const CompletionResult& completion) {
  return completion.finish_reason == FinishReason::BackendError;
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
  std::string line;

  while (true) {
    out << "you> ";
    out.flush();

    if (!std::getline(in, line)) {
      return result;
    }

    if (line.empty()) {
      continue;
    }

    conversation.messages.push_back({Role::User, line});
    ChatOutputSink sink(out);
    result.last_completion = backend.completeStreaming(
        makeConversationRequest(options, conversation), sink);

    if (completionFailed(result.last_completion)) {
      result.exit_code = 1;
      sink.finishError();
      reportBackendError(result.last_completion, err);
      return result;
    }

    sink.finishSuccess();
    conversation.messages.push_back(
        {Role::Assistant, result.last_completion.text});
  }
}

}  // namespace octopus
