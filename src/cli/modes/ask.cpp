#include "octopus/cli/ask.hpp"

#include <ostream>
#include <utility>

namespace octopus {
namespace {

// These prompts are the first Octopus policy layer. They are intentionally
// short: the harness should shape behavior without drowning out the user.
constexpr const char* kSystemPrompt = "You are Octopus, an AI agent for UNIX.";

constexpr const char* kDeveloperPrompt =
    "Answer the user's request directly. If you are unsure, say so.";

constexpr int kAskRepeatLastN = 64;
constexpr float kAskRepeatPenalty = 1.05F;

// Only transport/backend failures should make the CLI fail. Other finish
// reasons, including loop detection, can still produce user-visible text.
bool completionFailed(const llm::CompletionResult& completion) {
  return !completion.hasValue();
}

}  // namespace

llm::CompletionRequest makeConversationRequest(
    const CliOptions& options, const llm::Conversation& conversation) {
  llm::CompletionRequest request;
  // Keep the conversation structured as long as possible. Rendering to one
  // model-specific prompt string happens later in the backend.
  request.conversation.messages.push_back({llm::Role::System, kSystemPrompt});
  request.conversation.messages.push_back(
      {llm::Role::Developer, kDeveloperPrompt});
  request.conversation.messages.insert(request.conversation.messages.end(),
                                       conversation.messages.begin(),
                                       conversation.messages.end());
  // CLI requests are deterministic and conservative for now: useful for
  // testing, reproducibility, and reducing small-model repetition.
  request.generation.max_tokens = options.n_predict;
  request.generation.sampler_profile = llm::SamplerProfile::Deterministic;
  request.generation.quiet = options.quiet;
  request.generation.repeat_last_n = kAskRepeatLastN;
  request.generation.repeat_penalty = kAskRepeatPenalty;
  request.generation.frequency_penalty = 0.0F;
  request.generation.presence_penalty = 0.0F;
  return request;
}

llm::CompletionRequest makeAskRequest(const CliOptions& options) {
  llm::Conversation conversation;
  conversation.messages.push_back({llm::Role::User, options.prompt});
  return makeConversationRequest(options, conversation);
}

AskRunResult runOneShotAsk(const CliOptions& options, llm::Runtime& runtime,
                           std::ostream& out, std::ostream& err) {
  auto completion = runtime.complete(makeAskRequest(options));

  if (completionFailed(completion)) {
    err << (completion.error().empty() ? "LLM backend error"
                                       : completion.error())
        << '\n';
    return {1, std::move(completion)};
  }

  out << completion.value().response.text << '\n';
  return {0, std::move(completion)};
}

}  // namespace octopus
