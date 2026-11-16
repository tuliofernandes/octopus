#include "octopus/tool/tool.hpp"
#include "schema.hpp"

#include <fstream>
#include <regex>
#include <stdexcept>

namespace octopus::tool {
Definition loadManifest(const std::filesystem::path &root, const std::string &category,
                        const std::string &name) {
    /**
     * Each path component must start with a lowercase ASCII letter and
     * contain only lowercase letters, digits or underscores. The same
     * rule is enforced by Registry::add; together the components form
     * the category.name identity advertised to the model.
     */
    static const std::regex identifier("[a-z][a-z0-9_]*");
    if (!std::regex_match(category, identifier) || !std::regex_match(name, identifier))
        throw std::runtime_error("invalid tool identity");
    const auto path = root / category / name / "manifest.json";
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("cannot read manifest: " + path.string());
    try {
        auto data = nlohmann::json::parse(input);
        validateManifestEnvelope(data);
        if (!data.at("schema_version").is_number_integer() || data.at("schema_version") != 1)
            throw std::runtime_error("unsupported manifest version");
        Definition def{data.at("category").get<std::string>(), data.at("name").get<std::string>(),
                       data.at("description").get<std::string>(), data.at("parameters"),
                       data.at("permissions").get<std::vector<std::string>>()};
        if (def.category != category || def.name != name || def.description.empty())
            throw std::runtime_error("invalid manifest identity or description");
        /**
         * Only the schema root is checked here. It is exposed to the model;
         * the tool module supplies the validator that checks actual calls.
         */
        if (!def.parameters.is_object() ||
            def.parameters.value("type", nlohmann::json()) != "object")
            throw std::runtime_error("parameters must be an object schema");
        compileSchema(def.parameters);
        return def;
    } catch (const std::exception &e) {
        throw std::runtime_error("invalid manifest " + path.string() + ": " + e.what());
    }
}
} /* namespace octopus::tool */
