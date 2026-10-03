#pragma once

#include "octopus/llm/contracts.hpp"

#include <memory>
#include <string>
#include <vector>

namespace octopus::inference {

// Runtime knobs needed to load the local llama.cpp model. These are
// deliberately not exposed as general CLI flags yet; the harness owns the
// default policy.
struct LlamaCppBackendOptions {
  std::string model_path;
  int n_gpu_layers = 99;
  bool quiet = true;
};

// Concrete execution and metadata-template adapter for llama.cpp. The public
// type stays small; Impl owns the native resources and API details.
class LlamaCppBackend final : public llm::InferenceBackend,
                              public llm::ChatTemplateEngine {
 public:
  explicit LlamaCppBackend(LlamaCppBackendOptions options);
  ~LlamaCppBackend() override;

  LlamaCppBackend(const LlamaCppBackend&) = delete;
  LlamaCppBackend& operator=(const LlamaCppBackend&) = delete;
  LlamaCppBackend(LlamaCppBackend&&) = delete;
  LlamaCppBackend& operator=(LlamaCppBackend&&) = delete;

  llm::InferenceResult generate(const llm::CompiledPrompt& prompt,
                                const llm::GenerationOptions& generation,
                                const llm::CancellationToken* cancellation,
                                llm::CompletionSink* sink) override;
  llm::TemplateRenderResult render(
      const std::vector<llm::TemplateMessage>& messages) const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace octopus::inference
