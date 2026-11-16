#include "octopus/tool/tool.hpp"

#include "schema.hpp"

#include <exception>
#include <regex>
#include <stdexcept>
#include <utility>

namespace octopus::tool {

bool Registry::add(Definition definition, Handler handler, ArgumentValidator validator,
                   ActivityLabel label) {
    static const std::regex identifier("[a-z][a-z0-9_]*");
    if (!handler || !validator || !std::regex_match(definition.category, identifier) ||
        !std::regex_match(definition.name, identifier) || definition.description.empty() ||
        !definition.parameters.is_object() ||
        definition.parameters.value("type", nlohmann::json()) != "object")
        return false;
    try {
        auto generic = compileSchema(definition.parameters);
        validator = [generic, semantic = std::move(validator)](const auto &args) {
            auto diagnostic = generic(args);
            return diagnostic.empty() ? semantic(args) : diagnostic;
        };
    } catch (const std::exception &) {
        return false;
    }
    const auto identity = definition.identity();
    return entries_
        .emplace(identity, Entry{std::move(definition), std::move(handler), std::move(validator),
                                 std::move(label)})
        .second;
}

const Entry *Registry::find(const std::string &identity) const {
    const auto it = entries_.find(identity);
    return it == entries_.end() ? nullptr : &it->second;
}

std::vector<Definition> Registry::definitions() const {
    std::vector<Definition> definitions;
    for (const auto &entry : entries_)
        definitions.push_back(entry.second.definition);
    return definitions;
}

Result Executor::execute(const Call &call, const core::CancellationToken *cancellation,
                         ActivityObserver *observer) const {
    const auto *entry = registry_.find(call.identity);
    if (!entry)
        return {call.id, Status::UnknownTool, "unknown tool"};
    if (cancellation && cancellation->isCancellationRequested())
        return {call.id, Status::Cancelled, "cancelled"};
    /**
     * The registry composes schema and semantic validation. Authorization
     * follows both checks and precedes handler activity and execution.
     */
    try {
        auto diagnostic = entry->validator(call.arguments);
        if (!diagnostic.empty())
            return {call.id, Status::InvalidArguments, std::move(diagnostic)};
        auto decision = policy_.authorize(entry->definition, call);
        if (!decision.allowed)
            return {call.id, Status::Denied, std::move(decision.reason)};
        if (cancellation && cancellation->isCancellationRequested())
            return {call.id, Status::Cancelled, "cancelled"};
        ActivityStart event{call.id, call.identity, call.identity};
        try {
            if (entry->label)
                event.label = entry->label(call.arguments);
        } catch (...) {
        }
        if (observer) {
            try {
                observer->started(event);
            } catch (...) {
            }
        }
        Result result{call.id, Status::Failure, "handler failed"};
        const auto start = std::chrono::steady_clock::now();
        try {
            auto outcome = entry->handler(call.arguments, cancellation);
            result.duration = std::chrono::steady_clock::now() - start;
            result.status = outcome.status;
            result.output = std::move(outcome.output);
            result.summary = std::move(outcome.summary);
        } catch (const std::exception &error) {
            result.duration = std::chrono::steady_clock::now() - start;
            result.output = error.what();
        } catch (...) {
            result.duration = std::chrono::steady_clock::now() - start;
            result.output = "unknown handler exception";
        }
        if (observer) {
            try {
                observer->finished(event, result);
            } catch (...) {
            }
        }
        return result;
    } catch (const std::exception &error) {
        return {call.id, Status::Failure, error.what()};
    }
}

} /* namespace octopus::tool */
