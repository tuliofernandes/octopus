#pragma once

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

struct GenerationOptions {
  int max_tokens = 512;
  SamplerProfile sampler_profile = SamplerProfile::Deterministic;
  std::vector<std::string> stop_strings;
  bool quiet = true;
};

struct CompletionRequest {
  Conversation conversation;
  GenerationOptions generation;
};

enum class FinishReason {
  Stop,
  MaxTokens,
  EndOfGeneration,
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
