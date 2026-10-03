#include "octopus/llm/runtime.hpp"

namespace octopus::llm {
namespace {

CompletionResult rawResult(const RawCompletion& raw) {
  return CompletionResult::success(
      {AssistantResponse{raw.text}, raw.finish_reason, raw.generated_tokens});
}

}  // namespace

CompletionResult Runtime::complete(const CompletionRequest& request) {
  return completeImpl(request, nullptr);
}

CompletionResult Runtime::completeStreaming(const CompletionRequest& request,
                                            CompletionSink& sink) {
  return completeImpl(request, &sink);
}

CompletionResult Runtime::completeImpl(const CompletionRequest& request,
                                       CompletionSink* sink) {
  const auto validation = validateGenerationOptions(request.generation);
  if (!validation.ok) {
    return CompletionResult::failure(validation.error);
  }

  const auto compiled = integration_.compiler().compile(request.conversation);
  if (!compiled.hasValue()) {
    return CompletionResult::failure(compiled.error().empty()
                                         ? "prompt compilation failed"
                                         : compiled.error());
  }

  const auto generated = backend_.generate(compiled.value(), request.generation,
                                           request.cancellation, sink);
  if (!generated.hasValue()) {
    return CompletionResult::failure(
        generated.error().empty() ? "inference failed" : generated.error());
  }

  const auto& raw = generated.value();
  if (raw.finish_reason == FinishReason::Cancelled) {
    return rawResult(raw);
  }

  const auto parsed = integration_.parser().parse(raw);
  if (!parsed.hasValue()) {
    return CompletionResult::failure(parsed.error().empty()
                                         ? "assistant response parsing failed"
                                         : parsed.error());
  }

  return CompletionResult::success(
      {parsed.value(), raw.finish_reason, raw.generated_tokens});
}

}  // namespace octopus::llm
