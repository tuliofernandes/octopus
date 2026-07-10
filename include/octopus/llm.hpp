#pragma once

#include <cmath>
#include <string>
#include <vector>

namespace octopus {

/**
 * Chat roles are the harness-level vocabulary. They let Octopus keep intent
 * structured even though the backend eventually feeds the model one token
 * stream.
 */
enum class Role {
  System,
  Developer,
  User,
  Assistant,
};

struct Message {
  Role role = Role::User;
  std::string content;
};

struct Conversation {
  std::vector<Message> messages;
};

/**
 * The first harness keeps decoding deterministic so backend behavior is easier
 * to test and reason about before exposing creative sampling knobs.
 */
enum class SamplerProfile {
  Deterministic,
};

/**
 * Prompt rendering is model-specific: each chat model was trained with its own
 * role labels and delimiters, so generic Octopus messages need a renderer.
 */
enum class PromptRenderer {
  GemmaInstruction,
  LlamaChatTemplate,
};

/**
 * A fallback is separate from the preferred renderer so a model can prefer GGUF
 * metadata while still having a known manual format when metadata is absent.
 */
enum class PromptFallback {
  None,
  GemmaInstruction,
};

/**
 * ModelProfile is the compact "how this model wants to be spoken to" contract.
 * It keeps model quirks out of CLI and ask orchestration code.
 */
struct ModelProfile {
  PromptRenderer prompt_renderer = PromptRenderer::GemmaInstruction;
  PromptFallback fallback_renderer = PromptFallback::None;
  std::vector<std::string> stop_strings;
  /**
   * Some models do not support system/developer roles directly. Folding keeps
   * those instructions visible by placing them inside the next user turn.
   */
  bool fold_policy_messages = false;

  static ModelProfile gemmaInstruction();
  static ModelProfile llamaChatTemplate();
};

/**
 * GenerationOptions describe generic decoding policy. Backends translate these
 * fields to their own APIs, but callers should not need llama.cpp concepts.
 */
struct GenerationOptions {
  int max_tokens = 512;
  SamplerProfile sampler_profile = SamplerProfile::Deterministic;
  /**
   * Textual stops are chatbot boundaries: they prevent internal turn markers
   * from leaking into the user's visible answer.
   */
  std::vector<std::string> stop_strings;
  /**
   * Repeat penalties are a light guardrail against the raw model falling into
   * repetitive next-token loops.
   */
  int repeat_last_n = 0;
  float repeat_penalty = 1.0F;
  float frequency_penalty = 0.0F;
  float presence_penalty = 0.0F;
  bool quiet = true;
};

struct GenerationOptionsValidation {
  bool ok = true;
  std::string error;
};

inline bool repeatPenaltyEnabled(const GenerationOptions& options) {
  return options.repeat_penalty > 1.0F || options.frequency_penalty > 0.0F ||
         options.presence_penalty > 0.0F;
}

/**
 * Validate at the harness boundary so backend adapters can fail with a clear
 * error instead of passing nonsensical sampler parameters into native code.
 */
inline GenerationOptionsValidation validateGenerationOptions(
    const GenerationOptions& options) {
  if (options.max_tokens <= 0) {
    return {false, "max_tokens must be positive"};
  }
  if (options.repeat_last_n < 0) {
    return {false, "repeat_last_n must be non-negative"};
  }
  if (!std::isfinite(options.repeat_penalty) || options.repeat_penalty < 1.0F) {
    return {false, "repeat_penalty must be finite and at least 1.0"};
  }
  if (!std::isfinite(options.frequency_penalty) ||
      options.frequency_penalty < 0.0F) {
    return {false, "frequency_penalty must be finite and non-negative"};
  }
  if (!std::isfinite(options.presence_penalty) ||
      options.presence_penalty < 0.0F) {
    return {false, "presence_penalty must be finite and non-negative"};
  }
  if (repeatPenaltyEnabled(options) && options.repeat_last_n == 0) {
    return {false, "repeat_last_n must be positive when penalties are enabled"};
  }

  return {};
}

struct CompletionRequest {
  ModelProfile model_profile = ModelProfile::gemmaInstruction();
  Conversation conversation;
  GenerationOptions generation;
};

/**
 * FinishReason tells the caller why decoding stopped. That distinction matters
 * because a loop-trimmed answer can still be useful, while BackendError is not.
 */
enum class FinishReason {
  Stop,
  MaxTokens,
  EndOfGeneration,
  LoopDetected,
  BackendError,
};

struct CompletionResult {
  std::string text;
  FinishReason finish_reason = FinishReason::EndOfGeneration;
  int generated_tokens = 0;
  std::string error;
};

struct CompletionChunk {
  std::string text;
};

class CompletionSink {
 public:
  virtual ~CompletionSink() = default;
  virtual void onText(const CompletionChunk& chunk) = 0;
};

/**
 * LlmBackend is the seam between product behavior and a concrete inference
 * engine. Tests can use fakes; production currently uses llama.cpp.
 */
class LlmBackend {
 public:
  virtual ~LlmBackend() = default;
  virtual CompletionResult complete(const CompletionRequest& request) = 0;
  virtual CompletionResult completeStreaming(const CompletionRequest& request,
                                             CompletionSink& sink) {
    CompletionResult result = complete(request);
    if (result.finish_reason != FinishReason::BackendError &&
        !result.text.empty()) {
      sink.onText({result.text});
    }
    return result;
  }
};

}  // namespace octopus
