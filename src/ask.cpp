#include "octopus/ask.hpp"

#include <ostream>

namespace octopus {
namespace {

constexpr const char *kSystemPrompt =
    "You are Octopus, a concise CLI AI agent for UNIX/Linux.";

constexpr const char *kDeveloperPrompt =
    "Answer the user's request directly. If you are unsure, say so.";

constexpr int kAskRepeatLastN = 64;
constexpr float kAskRepeatPenalty = 1.05F;

bool completion_failed(const CompletionResult &completion) {
  return completion.finish_reason == FinishReason::BackendError;
}

} // namespace

CompletionRequest make_ask_request(const CliOptions &options) {
  return make_ask_request(options, ModelProfile::gemma_instruction());
}

CompletionRequest make_ask_request(const CliOptions &options,
                                   const ModelProfile &profile) {
  CompletionRequest request;
  request.model_profile = profile;
  request.conversation.messages.push_back({Role::System, kSystemPrompt});
  request.conversation.messages.push_back({Role::Developer, kDeveloperPrompt});
  request.conversation.messages.push_back({Role::User, options.prompt});
  request.generation.max_tokens = options.n_predict;
  request.generation.sampler_profile = SamplerProfile::Deterministic;
  request.generation.quiet = options.quiet;
  request.generation.stop_strings = profile.stop_strings;
  request.generation.repeat_last_n = kAskRepeatLastN;
  request.generation.repeat_penalty = kAskRepeatPenalty;
  request.generation.frequency_penalty = 0.0F;
  request.generation.presence_penalty = 0.0F;
  return request;
}

AskRunResult run_one_shot_ask(const CliOptions &options, LlmBackend &backend,
                              std::ostream &out, std::ostream &err) {
  AskRunResult result;
  result.completion = backend.complete(make_ask_request(options));

  if (completion_failed(result.completion)) {
    result.exit_code = 1;
    err << (result.completion.error.empty() ? "LLM backend error"
                                            : result.completion.error)
        << '\n';
    return result;
  }

  out << result.completion.text << '\n';
  return result;
}

} // namespace octopus
