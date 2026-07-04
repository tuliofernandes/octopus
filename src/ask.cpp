#include "octopus/ask.hpp"

#include <ostream>

namespace octopus {
namespace {

/**
 * These prompts are the first Octopus policy layer. They are intentionally
 * short: the harness should shape behavior without drowning out the user.
 */
constexpr const char *kSystemPrompt =
    "You are Octopus, an AI agent for UNIX.";

constexpr const char *kDeveloperPrompt =
    "Answer the user's request directly. If you are unsure, say so.";

constexpr int kAskRepeatLastN = 64;
constexpr float kAskRepeatPenalty = 1.05F;

/**
 * Only transport/backend failures should make the CLI fail. Other finish
 * reasons, including loop detection, can still produce user-visible text.
 */
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
  /**
   * Keep the conversation structured as long as possible. Rendering to one
   * model-specific prompt string happens later in the backend.
   */
  request.conversation.messages.push_back({Role::System, kSystemPrompt});
  request.conversation.messages.push_back({Role::Developer, kDeveloperPrompt});
  request.conversation.messages.push_back({Role::User, options.prompt});
  /**
   * CLI ask is deterministic and conservative for now: useful for testing,
   * reproducibility, and reducing small-model repetition.
   */
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
  /**
   * The runner depends only on LlmBackend, so tests can exercise CLI behavior
   * with a fake backend instead of loading a real GGUF model.
   */
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
