#pragma once

#include <cmath>
#include <string>
#include <vector>

namespace octopus {

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

enum class SamplerProfile {
  Deterministic,
};

enum class PromptRenderer {
  GemmaInstruction,
  LlamaChatTemplate,
};

enum class PromptFallback {
  None,
  GemmaInstruction,
};

struct ModelProfile {
  PromptRenderer prompt_renderer = PromptRenderer::GemmaInstruction;
  PromptFallback fallback_renderer = PromptFallback::None;
  std::vector<std::string> stop_strings;
  bool fold_policy_messages = false;

  static ModelProfile gemma_instruction();
  static ModelProfile llama_chat_template();
};

struct GenerationOptions {
  int max_tokens = 512;
  SamplerProfile sampler_profile = SamplerProfile::Deterministic;
  std::vector<std::string> stop_strings;
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

inline bool repeat_penalty_enabled(const GenerationOptions &options) {
  return options.repeat_penalty > 1.0F || options.frequency_penalty > 0.0F ||
         options.presence_penalty > 0.0F;
}

inline GenerationOptionsValidation
validate_generation_options(const GenerationOptions &options) {
  if (options.max_tokens <= 0) {
    return {false, "max_tokens must be positive"};
  }
  if (options.repeat_last_n < 0) {
    return {false, "repeat_last_n must be non-negative"};
  }
  if (!std::isfinite(options.repeat_penalty) ||
      options.repeat_penalty < 1.0F) {
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
  if (repeat_penalty_enabled(options) && options.repeat_last_n == 0) {
    return {false, "repeat_last_n must be positive when penalties are enabled"};
  }

  return {};
}

struct CompletionRequest {
  ModelProfile model_profile = ModelProfile::gemma_instruction();
  Conversation conversation;
  GenerationOptions generation;
};

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

class LlmBackend {
public:
  virtual ~LlmBackend() = default;
  virtual CompletionResult complete(const CompletionRequest &request) = 0;
};

} // namespace octopus
