// plugin_host.cpp — dlopen-based plugin loader.
#include "plugin_host.hpp"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace chce {
namespace {

bool has_so_suffix(const std::string& p) {
  return p.size() > 3 && p.compare(p.size() - 3, 3, ".so") == 0;
}

}  // namespace

std::string default_plugin_dir() {
  if (const char* env = std::getenv("CHCE_PLUGIN_DIR")) return env;
  return "plugins";
}

std::vector<LoadedPlugin> load_plugins(const std::string& dir) {
  std::vector<LoadedPlugin> out;
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    // Only complain when the user asked for a specific directory. A missing
    // default ./plugins is the normal case for a build without plugins.
    if (std::getenv("CHCE_PLUGIN_DIR")) {
      std::fprintf(stderr, "chce: plugin directory '%s' not found\n", dir.c_str());
    }
    return out;
  }
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    const std::string name = entry.path().filename().string();
    if (!has_so_suffix(name)) continue;
    const std::string path = entry.path().string();
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
      std::fprintf(stderr, "chce: plugin %s not loaded: %s\n", path.c_str(), dlerror());
      continue;
    }
    auto entry_fn = reinterpret_cast<chce_plugin_entry_fn>(dlsym(h, CHCE_PLUGIN_ENTRY));
    if (!entry_fn) {
      std::fprintf(stderr, "chce: plugin %s has no %s\n", path.c_str(), CHCE_PLUGIN_ENTRY);
      dlclose(h);
      continue;
    }
    const chce_plugin* api = entry_fn();
    if (!api || api->abi_version != CHCE_PLUGIN_ABI || !api->compress || !api->decompress ||
        !api->bound) {
      std::fprintf(stderr, "chce: plugin %s has an incompatible ABI\n", path.c_str());
      dlclose(h);
      continue;
    }
    LoadedPlugin lp;
    lp.path = path;
    lp.name = api->name ? api->name : name;
    lp.licence = api->licence ? api->licence : "unknown";
    lp.api = api;
    lp.handle = h;
    out.push_back(std::move(lp));
  }
  return out;
}

}  // namespace chce
