#pragma once
#include "octopus/core/cancellation.hpp"
#include "octopus/tool/activity.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace octopus::tool {

struct Definition {
    std::string category, name, description;
    nlohmann::json parameters;
    std::vector<std::string> permissions;

    std::string identity() const { return category + "." + name; }
};

Definition loadManifest(const std::filesystem::path &root, const std::string &category,
                        const std::string &name);

struct Call {
    std::string id, identity;
    nlohmann::json arguments;
};
enum class Status { Success, UnknownTool, InvalidArguments, Denied, Failure, Cancelled };

/**
 * Handlers report operational Success, Failure or Cancelled; lookup,
 * validation and authorization statuses belong to the executor. A process
 * tool may successfully observe a nonzero command exit. Summary is optional
 * activity metadata and is not part of the JSON output sent to the model.
 */
struct Outcome {
    Status status;
    nlohmann::json output;
    std::optional<std::string> summary;

    Outcome(Status status, nlohmann::json output, std::optional<std::string> summary = std::nullopt)
        : status(status), output(std::move(output)), summary(std::move(summary)) {}

    Outcome(std::string text) : Outcome(Status::Success, std::move(text)) {}

    Outcome(const char *text) : Outcome(std::string(text)) {}
};

using ActivityLabel = std::function<std::string(const nlohmann::json &)>;

struct Result {
    std::string call_id;
    Status status;
    nlohmann::json output;
    std::optional<std::chrono::steady_clock::duration> duration = std::nullopt;
    std::optional<std::string> summary = std::nullopt;
};

using Handler = std::function<Outcome(const nlohmann::json &, const core::CancellationToken *)>;
/**
 * Return an empty string for valid arguments or a diagnostic otherwise.
 */
using ArgumentValidator = std::function<std::string(const nlohmann::json &)>;

struct Entry {
    Definition definition;
    Handler handler;
    ArgumentValidator validator;
    ActivityLabel label;
};

class Registry {
  public:
    bool add(Definition definition, Handler handler, ArgumentValidator validator,
             ActivityLabel label = {});
    const Entry *find(const std::string &identity) const;
    std::vector<Definition> definitions() const;

  private:
    std::map<std::string, Entry> entries_;
};

/**
 * Discover manifests and load their accompanying in-process libraries.
 */
void loadTools(Registry &registry, const std::filesystem::path &root);

struct AuthorizationDecision {
    bool allowed;
    std::string reason;
};

class SecurityPolicy {
  public:
    virtual ~SecurityPolicy() = default;
    virtual AuthorizationDecision authorize(const Definition &, const Call &) const = 0;
};

class AllowAllSecurityPolicy final : public SecurityPolicy {
  public:
    AuthorizationDecision authorize(const Definition &, const Call &) const override {
        return {true, {}};
    }
};

class Executor {
  public:
    Executor(const Registry &registry, const SecurityPolicy &policy)
        : registry_(registry), policy_(policy) {}

    Result execute(const Call &call, const core::CancellationToken *cancellation = nullptr,
                   ActivityObserver *observer = nullptr) const;

  private:
    const Registry &registry_;
    const SecurityPolicy &policy_;
};

} /* namespace octopus::tool */
