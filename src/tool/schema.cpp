#include "schema.hpp"

#include <memory>
#include <nlohmann/json-schema.hpp>
#include <stdexcept>

namespace octopus::tool {

ArgumentValidator compileSchema(const nlohmann::json &schema) {
    if (!schema.is_object() || schema.value("type", nlohmann::json()) != "object")
        throw std::runtime_error("parameters must be an object schema");
    if (schema.contains("$schema") &&
        schema.at("$schema") != "http://json-schema.org/draft-07/schema#")
        throw std::runtime_error("unsupported schema dialect; expected Draft-07");
    /**
     * Compilation alone does not reject every malformed schema. Validate
     * against the embedded metaschema first. With no external URI loader,
     * references stay local; the no-op format checker makes format an annotation.
     */
    nlohmann::json_schema::json_validator meta(nullptr,
                                               [](const std::string &, const std::string &) {});
    meta.set_root_schema(nlohmann::json_schema::draft7_schema_builtin);
    meta.validate(schema);
    auto compiled = std::make_shared<nlohmann::json_schema::json_validator>(
        nullptr, [](const std::string &, const std::string &) {});
    compiled->set_root_schema(schema);
    /**
     * The registry retains this compiled validator through shared ownership.
     * Ignore the returned default-value patch: validation must not rewrite
     * the arguments that the policy and handler receive.
     */
    return [compiled](const nlohmann::json &arguments) -> std::string {
        try {
            compiled->validate(arguments);
            return {};
        } catch (const std::exception &error) {
            return error.what();
        }
    };
}

void validateManifestEnvelope(const nlohmann::json &manifest) {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"properties",
         {{"schema_version", {{"type", "integer"}, {"const", 1}}},
          {"category", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"description", {{"type", "string"}, {"minLength", 1}}},
          {"parameters", {{"type", "object"}}},
          {"permissions", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
        {"required",
         {"schema_version", "category", "name", "description", "parameters", "permissions"}},
        {"additionalProperties", false}};
    const auto diagnostic = compileSchema(schema)(manifest);
    if (!diagnostic.empty())
        throw std::runtime_error(diagnostic);
}

} /* namespace octopus::tool */
