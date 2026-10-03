#pragma once

#include "octopus/llm/contracts.hpp"

namespace octopus::models::google::gemma {

class GemmaPromptCompiler final : public llm::PromptCompiler {
 public:
  explicit GemmaPromptCompiler(const llm::ChatTemplateEngine& template_engine)
      : template_engine_(template_engine) {}

  llm::CompileResult compile(
      const llm::Conversation& conversation) const override;

 private:
  const llm::ChatTemplateEngine& template_engine_;
};

class GemmaResponseParser final : public llm::AssistantResponseParser {
 public:
  llm::ParseResult parse(const llm::RawCompletion& completion) const override;
};

class GemmaIntegration final {
 public:
  explicit GemmaIntegration(const llm::ChatTemplateEngine& template_engine);

  GemmaIntegration(const GemmaIntegration&) = delete;
  GemmaIntegration& operator=(const GemmaIntegration&) = delete;
  GemmaIntegration(GemmaIntegration&&) = delete;
  GemmaIntegration& operator=(GemmaIntegration&&) = delete;

  const llm::ModelIntegration& modelIntegration() const noexcept {
    return integration_;
  }

 private:
  GemmaPromptCompiler compiler_;
  GemmaResponseParser parser_;
  llm::ModelIntegration integration_;
};

}  // namespace octopus::models::google::gemma
