#include "octopus/chat/runtime.hpp"

#include <string>

namespace octopus::chat {

namespace {

const char *statusName(tool::Status status) {
    switch (status) {
    case tool::Status::Success:
        return "success";
    case tool::Status::UnknownTool:
        return "unknown_tool";
    case tool::Status::InvalidArguments:
        return "invalid_arguments";
    case tool::Status::Denied:
        return "denied";
    case tool::Status::Failure:
        return "failure";
    case tool::Status::Cancelled:
        return "cancelled";
    }

    return "failure";
}

} /* namespace */

llm::CompletionResult Runtime::turn(const std::string &text,
                                    const core::CancellationToken *cancellation,
                                    llm::CompletionSink *sink) {
    history_.messages.push_back({llm::Role::User, text});
    bool accepted = false;
    std::size_t rounds = 0;

    while (true) {
        auto request = base_;
        request.conversation.messages.insert(request.conversation.messages.end(),
                                             history_.messages.begin(), history_.messages.end());
        request.conversation.tools = registry_.definitions();
        request.cancellation = cancellation;

        auto completion = sink ? llm_.completeStreaming(request, *sink) : llm_.complete(request);

        if (!completion.hasValue()) {
            if (!accepted)
                history_.messages.pop_back();
            return completion;
        }

        if (completion.value().finish_reason == llm::FinishReason::Cancelled) {
            if (!accepted)
                history_.messages.pop_back();
            return completion;
        }

        auto &response = completion.value().response;
        if (response.tool_calls.empty()) {
            history_.messages.push_back({llm::Role::Assistant, response.text});
            return completion;
        }

        if (rounds++ >= round_limit_)
            return llm::CompletionResult::failure("tool round limit reached");

        for (auto &call : response.tool_calls)
            call.id = "call-" + std::to_string(next_id_++);

        history_.messages.push_back({llm::Role::Assistant, response.text, response.tool_calls, {}});
        accepted = true;

        bool cancelled = false;
        for (const auto &call : response.tool_calls) {
            auto result = cancelled ? tool::Result{call.id, tool::Status::Cancelled, "cancelled"}
                                    : executor_.execute(call, cancellation);
            cancelled = cancelled || result.status == tool::Status::Cancelled;

            nlohmann::json payload = {{"status", statusName(result.status)},
                                      {"output", result.output}};
            history_.messages.push_back({llm::Role::Tool, payload.dump(), {}, call.id});
        }

        if (cancelled || (cancellation && cancellation->isCancellationRequested())) {
            return llm::CompletionResult::success({{}, llm::FinishReason::Cancelled, 0});
        }
    }
}

} /* namespace octopus::chat */
