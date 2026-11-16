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
                                    llm::CompletionSink *sink, tool::ActivityObserver *observer,
                                    const ToolTrace &trace,
                                    const GenerationTrace &generation_trace) {
    history_.messages.push_back({llm::Role::User, text});
    bool accepted = false;
    std::size_t rounds = 0;

    while (true) {
        /**
         * Tool calling starts by giving the model a catalog of available tools.
         * registry_.definitions() provides their names, descriptions and parameter
         * schemas; the model integration and chat template add them to the prompt.
         * For example, one internal definition can describe:
         *   identity:   "misc.hello_world"
         *   description: "Returns a greeting and a random number."
         *   parameters: {"type":"object", "properties":{}, "additionalProperties":false}
         * The executable handlers stay in the registry and are not sent to the model.
         * This is an illustration of structured data; the GGUF chat template
         * decides how the catalog is written into the actual prompt.
         *
         * The request also includes the session history. If the model asks to call
         * a tool, the code below executes it and appends the outcome to that history.
         * The next iteration sends both the call and its result back to the model.
         */
        auto request = base_;
        request.conversation.messages.insert(request.conversation.messages.end(),
                                             history_.messages.begin(), history_.messages.end());
        request.conversation.tools = registry_.definitions();
        request.cancellation = cancellation;

        const auto start = generation_trace ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
        auto completion = sink ? llm_.completeStreaming(request, *sink) : llm_.complete(request);
        if (generation_trace) {
            const auto elapsed = std::chrono::steady_clock::now() - start;
            try {
                generation_trace(elapsed, completion);
            } catch (...) {
            }
        }

        /**
         * Before a tool round is accepted, failure or cancellation discards the
         * pending user message. Afterwards, keep the history of accepted work.
         */
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

        /**
         * llm::Runtime has already passed the generated text to the active
         * model parser. An empty call list completes the turn; otherwise these
         * structured calls are requests to execute tools, not final output.
         */
        auto &response = completion.value().response;
        if (response.tool_calls.empty()) {
            history_.messages.push_back({llm::Role::Assistant, response.text});
            return completion;
        }

        if (rounds++ >= round_limit_)
            return llm::CompletionResult::failure("tool round limit reached");

        /**
         * The runtime, not the model parser, assigns session-unique IDs. The
         * assistant calls and their tool results share these IDs in history.
         */
        for (auto &call : response.tool_calls)
            call.id = "call-" + std::to_string(next_id_++);

        history_.messages.push_back({llm::Role::Assistant, response.text, response.tool_calls, {}});
        accepted = true;

        /**
         * Record a result for every accepted call, even when cancellation
         * prevents later calls from executing.
         */
        bool cancelled = false;
        for (const auto &call : response.tool_calls) {
            auto result = cancelled ? tool::Result{call.id, tool::Status::Cancelled, "cancelled"}
                                    : executor_.execute(call, cancellation, observer);
            cancelled = cancelled || result.status == tool::Status::Cancelled;

            /**
             * Feed both successful and unsuccessful outcomes back as tool
             * messages. Their call IDs link them to the assistant message above;
             * the next loop iteration resends this history so the model can
             * continue with the results or request another round. For example,
             * after call-1 invokes misc.hello_world with {}, history contains:
             *   role: tool
             *   tool_call_id: "call-1"
             *   content: {"status":"success", "output":"Hello, world! Random number 12345"}
             * Here content is a JSON-encoded string; the template determines
             * its final representation in the next prompt. A failure uses the
             * same fields with a different status and diagnostic output.
             */
            nlohmann::json payload = {{"status", statusName(result.status)},
                                      {"output", result.output}};
            if (trace) {
                try {
                    trace(call, payload, result.duration);
                } catch (...) {
                }
            }
            history_.messages.push_back({llm::Role::Tool, payload.dump(), {}, call.id});
        }

        if (cancelled || (cancellation && cancellation->isCancellationRequested())) {
            return llm::CompletionResult::success({{}, llm::FinishReason::Cancelled, 0});
        }
    }
}

} /* namespace octopus::chat */
