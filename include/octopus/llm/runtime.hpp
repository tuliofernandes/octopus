#pragma once

#include "octopus/llm/contracts.hpp"

namespace octopus::llm {

class Runtime final {
 public:
  Runtime(const ModelIntegration& integration, InferenceBackend& backend)
      : integration_(integration), backend_(backend) {}

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime(Runtime&&) = delete;
  Runtime& operator=(Runtime&&) = delete;

  CompletionResult complete(const CompletionRequest& request);
  CompletionResult completeStreaming(const CompletionRequest& request,
                                     CompletionSink& sink);

  const ModelCapabilities& capabilities() const noexcept {
    return integration_.capabilities();
  }

 private:
  CompletionResult completeImpl(const CompletionRequest& request,
                                CompletionSink* sink);

  const ModelIntegration& integration_;
  InferenceBackend& backend_;
};

}  // namespace octopus::llm
