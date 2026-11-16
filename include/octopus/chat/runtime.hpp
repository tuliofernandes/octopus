#pragma once
#include "octopus/llm/runtime.hpp"
#include "octopus/tool/tool.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>

namespace octopus::chat {

/* Optional synchronous diagnostic callback; it receives the history payload. */
using ToolTrace = std::function<void(const tool::Call &, const nlohmann::json &,
                                     std::optional<std::chrono::steady_clock::duration>)>;
using GenerationTrace =
    std::function<void(std::chrono::steady_clock::duration, const llm::CompletionResult &)>;

class Runtime final {
  public:
    Runtime(llm::Runtime &llm, const tool::Registry &registry, const tool::Executor &executor,
            llm::CompletionRequest base, std::size_t round_limit = 8)
        : llm_(llm), registry_(registry), executor_(executor), base_(std::move(base)),
          round_limit_(round_limit) {}

    llm::CompletionResult turn(const std::string &text, const core::CancellationToken *cancellation,
                               llm::CompletionSink *sink = nullptr,
                               tool::ActivityObserver *observer = nullptr,
                               const ToolTrace &trace = {},
                               const GenerationTrace &generation_trace = {});

    const llm::Conversation &history() const { return history_; }

  private:
    llm::Runtime &llm_;
    const tool::Registry &registry_;
    const tool::Executor &executor_;
    llm::CompletionRequest base_;
    std::size_t round_limit_;
    std::size_t next_id_ = 1;
    llm::Conversation history_;
};

} /* namespace octopus::chat */
