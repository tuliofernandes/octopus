#pragma once

#include <cmath>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace octopus::llm {

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

class CancellationToken {
 public:
  virtual ~CancellationToken() = default;
  virtual bool isCancellationRequested() const noexcept = 0;
};

inline bool repeatPenaltyEnabled(const GenerationOptions& options) {
  return options.repeat_penalty > 1.0F || options.frequency_penalty > 0.0F ||
         options.presence_penalty > 0.0F;
}

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

enum class FinishReason {
  Stop,
  MaxTokens,
  EndOfGeneration,
  LoopDetected,
  Cancelled,
};

template <typename Value>
class Result final {
 public:
  static Result success(Value value) { return Result(std::move(value)); }

  static Result failure(std::string error) {
    return Result(Error{std::move(error)});
  }

  bool hasValue() const noexcept {
    return std::holds_alternative<Value>(state_);
  }

  explicit operator bool() const noexcept { return hasValue(); }

  const Value& value() const { return std::get<Value>(state_); }
  Value& value() { return std::get<Value>(state_); }

  const std::string& error() const { return std::get<Error>(state_).message; }

 private:
  struct Error {
    std::string message;
  };

  explicit Result(Value value) : state_(std::move(value)) {}
  explicit Result(Error error) : state_(std::move(error)) {}

  std::variant<Value, Error> state_;
};

struct RawCompletion {
  std::string text;
  FinishReason finish_reason = FinishReason::EndOfGeneration;
  int generated_tokens = 0;
};

struct AssistantResponse {
  std::string text;
};

struct Completion {
  AssistantResponse response;
  FinishReason finish_reason = FinishReason::EndOfGeneration;
  int generated_tokens = 0;
};

using InferenceResult = Result<RawCompletion>;
using CompletionResult = Result<Completion>;

struct CompletionChunk {
  std::string text;
};

class CompletionSink {
 public:
  virtual ~CompletionSink() = default;
  virtual void onText(const CompletionChunk& chunk) = 0;
};

struct CompletionRequest {
  Conversation conversation;
  GenerationOptions generation;
  const CancellationToken* cancellation = nullptr;
};

}  // namespace octopus::llm
