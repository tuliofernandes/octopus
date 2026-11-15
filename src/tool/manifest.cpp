#include "octopus/tool/tool.hpp"

#include <fstream>
#include <regex>
#include <stdexcept>

namespace octopus::tool {
Definition loadManifest(const std::filesystem::path &root, const std::string &category,
                        const std::string &name) {
    static const std::regex identifier("[a-z][a-z0-9_]*");
    if (!std::regex_match(category, identifier) || !std::regex_match(name, identifier))
        throw std::runtime_error("invalid tool identity");
    const auto path = root / category / name / "manifest.json";
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("cannot read manifest: " + path.string());
    try {
        auto data = nlohmann::json::parse(input);
        if (!data.at("schema_version").is_number_integer() || data.at("schema_version") != 1)
            throw std::runtime_error("unsupported manifest version");
        Definition def{data.at("category").get<std::string>(), data.at("name").get<std::string>(),
                       data.at("description").get<std::string>(), data.at("parameters"),
                       data.at("permissions").get<std::vector<std::string>>()};
        if (def.category != category || def.name != name || def.description.empty())
            throw std::runtime_error("invalid manifest identity or description");
        if (!def.parameters.is_object() ||
            def.parameters.value("type", nlohmann::json()) != "object")
            throw std::runtime_error("parameters must be an object schema");
        return def;
    } catch (const std::exception &e) {
        throw std::runtime_error("invalid manifest " + path.string() + ": " + e.what());
    }
}
} // namespace octopus::tool
