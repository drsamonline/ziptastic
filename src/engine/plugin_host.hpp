// plugin_host.hpp — loads backend plugins from a directory at runtime.
#pragma once

#include <string>
#include <vector>

#include "chimera/plugin.hpp"

namespace chce {

struct LoadedPlugin {
  std::string path;
  std::string name;
  std::string licence;
  const chce_plugin* api = nullptr;
  void* handle = nullptr;
};

// Scans `dir` for shared objects, loads each, and keeps those that expose a
// compatible ABI. Never throws; a bad plugin is skipped and reported in the log.
std::vector<LoadedPlugin> load_plugins(const std::string& dir);

// Convenience: the default plugin directory next to the executable, overridable
// with CHCE_PLUGIN_DIR.
std::string default_plugin_dir();

}  // namespace chce
