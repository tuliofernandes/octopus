#pragma once

#include "octopus/llm.hpp"

#include <memory>
#include <string>

namespace octopus {

/**
 * Runtime knobs needed to load the local llama.cpp model. These are deliberately
 * not exposed as general CLI flags yet; the harness owns the default policy.
 */
struct LlamaCppBackendOptions {
  std::string model_path;
  int n_gpu_layers = 99;
  bool quiet = true;
};

/**
 * Concrete adapter from the generic LlmBackend contract to llama.cpp. The
 * public type stays small; Impl owns the native resources and API details.
 */
class LlamaCppBackend final : public LlmBackend {
public:
  explicit LlamaCppBackend(LlamaCppBackendOptions options);
  ~LlamaCppBackend() override;

  LlamaCppBackend(const LlamaCppBackend &) = delete;
  LlamaCppBackend &operator=(const LlamaCppBackend &) = delete;
  LlamaCppBackend(LlamaCppBackend &&) noexcept;
  LlamaCppBackend &operator=(LlamaCppBackend &&) noexcept;

  CompletionResult complete(const CompletionRequest &request) override;
  CompletionResult complete_streaming(const CompletionRequest &request,
                                      CompletionSink &sink) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace octopus
