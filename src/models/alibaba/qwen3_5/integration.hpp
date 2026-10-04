#pragma once

#include "octopus/llm/contracts.hpp"

namespace octopus::models::alibaba::qwen3_5 {

class Qwen35PromptCompiler final : public llm::PromptCompiler {
 public:
  explicit Qwen35PromptCompiler(const llm::ChatTemplateEngine& template_engine)
      : template_engine_(template_engine) {}

  llm::CompileResult compile(
      const llm::Conversation& conversation) const override;

 private:
  const llm::ChatTemplateEngine& template_engine_;
};

class Qwen35ResponseParser final : public llm::AssistantResponseParser {
 public:
  llm::ParseResult parse(const llm::RawCompletion& completion) const override;
};

class Qwen35Integration final {
 public:
  explicit Qwen35Integration(const llm::ChatTemplateEngine& template_engine);

  Qwen35Integration(const Qwen35Integration&) = delete;
  Qwen35Integration& operator=(const Qwen35Integration&) = delete;
  Qwen35Integration(Qwen35Integration&&) = delete;
  Qwen35Integration& operator=(Qwen35Integration&&) = delete;

  const llm::ModelIntegration& modelIntegration() const noexcept {
    return integration_;
  }

 private:
  Qwen35PromptCompiler compiler_;
  Qwen35ResponseParser parser_;
  llm::ModelIntegration integration_;
};

}  // namespace octopus::models::alibaba::qwen3_5
