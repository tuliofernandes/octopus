#pragma once
#include "octopus/core/cancellation.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
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

struct Result {
    std::string call_id;
    Status status;
    std::string output;
};

using Handler = std::function<std::string(const nlohmann::json &, const core::CancellationToken *)>;
/* Returns an empty string for valid arguments or a diagnostic otherwise. */
using ArgumentValidator = std::function<std::string(const nlohmann::json &)>;

struct Entry {
    Definition definition;
    Handler handler;
    ArgumentValidator validator;
};

class Registry {
  public:
    bool add(Definition definition, Handler handler, ArgumentValidator validator);
    const Entry *find(const std::string &identity) const;
    std::vector<Definition> definitions() const;

  private:
    std::map<std::string, Entry> entries_;
};

/* Discover manifests and load their accompanying in-process libraries. */
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

    Result execute(const Call &call, const core::CancellationToken *cancellation = nullptr) const;

  private:
    const Registry &registry_;
    const SecurityPolicy &policy_;
};
} /* namespace octopus::tool */
