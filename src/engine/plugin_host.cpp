// plugin_host.cpp — plugin loader.
//
// POSIX uses dlopen/dlsym/dlclose; Windows uses LoadLibrary/GetProcAddress/FreeLibrary.
// The two are close enough that the only things that differ are the include, the suffix
// a plugin carries, and the three calls — so the loop below is written once.
#include "plugin_host.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace chce {
namespace {

#ifdef _WIN32
constexpr const char* kPluginSuffix = ".dll";
#else
constexpr const char* kPluginSuffix = ".so";
#endif

bool has_plugin_suffix(const std::string& p) {
  const std::string suffix = kPluginSuffix;
  return p.size() > suffix.size() &&
         p.compare(p.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The three calls, wrapped so the loader below reads the same on both platforms.
void* open_library(const std::string& path, std::string& error) {
#ifdef _WIN32
  HMODULE handle = LoadLibraryA(path.c_str());
  if (handle == nullptr) {
    error = "error " + std::to_string(GetLastError());
    return nullptr;
  }
  return reinterpret_cast<void*>(handle);
#else
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) error = dlerror() ? dlerror() : "unknown error";
  return handle;
#endif
}

void* library_symbol(void* handle, const char* name) {
#ifdef _WIN32
  return reinterpret_cast<void*>(
      GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
  return dlsym(handle, name);
#endif
}

void close_library(void* handle) {
#ifdef _WIN32
  FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
  dlclose(handle);
#endif
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
    if (!has_plugin_suffix(name)) continue;
    const std::string path = entry.path().string();
    std::string open_error;
    void* h = open_library(path, open_error);
    if (h == nullptr) {
      std::fprintf(stderr, "chce: plugin %s not loaded: %s\n", path.c_str(),
                   open_error.c_str());
      continue;
    }
    auto entry_fn = reinterpret_cast<chce_plugin_entry_fn>(library_symbol(h, CHCE_PLUGIN_ENTRY));
    if (!entry_fn) {
      std::fprintf(stderr, "chce: plugin %s has no %s\n", path.c_str(), CHCE_PLUGIN_ENTRY);
      close_library(h);
      continue;
    }
    const chce_plugin* api = entry_fn();
    if (!api || api->abi_version != CHCE_PLUGIN_ABI || !api->compress || !api->decompress ||
        !api->bound) {
      std::fprintf(stderr, "chce: plugin %s has an incompatible ABI\n", path.c_str());
      close_library(h);
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
