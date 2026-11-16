#include "octopus/tool/module.hpp"
#include "octopus/tool/tool.hpp"

#include <algorithm>
#include <dlfcn.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace octopus::tool {

void loadTools(Registry &registry, const std::filesystem::path &root) {
    namespace fs = std::filesystem;
    std::vector<fs::path> manifests;
    for (const auto &category : fs::directory_iterator(root)) {
        if (!category.is_directory())
            continue;
        for (const auto &tool : fs::directory_iterator(category.path())) {
            if (tool.is_directory() && fs::exists(tool.path() / "manifest.json"))
                manifests.push_back(tool.path());
        }
    }
    std::sort(manifests.begin(), manifests.end());
    for (const auto &dir : manifests) {
        auto definition =
            loadManifest(root, dir.parent_path().filename().string(), dir.filename().string());
        const auto path = dir / (std::string("tool") + OCTOPUS_MODULE_SUFFIX);
        void *raw = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!raw)
            throw std::runtime_error("cannot load tool " + path.string() + ": " + dlerror());
        /**
         * Every registered callback, including the optional label, retains the
         * library so its function pointer stays valid for the registry lifetime.
         */
        auto library = std::shared_ptr<void>(raw, [](void *p) { dlclose(p); });
        dlerror();
        auto entry = reinterpret_cast<ToolModuleEntry>(dlsym(raw, "octopusToolModule"));
        const char *error = dlerror();
        if (error || !entry)
            throw std::runtime_error("invalid tool entry point: " + path.string());
        const ToolModule *module = entry();
        /**
         * Read the common version field before any v2-only member. A v1 execute
         * callback has an incompatible signature and must never be invoked as v2.
         */
        if (!module || module->version != kToolModuleVersion || !module->validate ||
            !module->execute)
            throw std::runtime_error("invalid tool module (expected ABI v2): " + path.string());
        auto validate = module->validate;
        auto execute = module->execute;
        auto label = module->label;
        if (!registry.add(
                std::move(definition),
                [library, execute](const auto &args, const auto *cancel) {
                    return execute(args, cancel);
                },
                [library, validate](const auto &args) { return validate(args); },
                label ? ActivityLabel([library, label](const auto &args) { return label(args); })
                      : ActivityLabel{}))
            throw std::runtime_error("failed to register tool: " + dir.string());
    }
}

} /* namespace octopus::tool */
