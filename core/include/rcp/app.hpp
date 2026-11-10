// myrcp's public API: run a workbench for an application built on it.
//
// An application (a "product", as Eclipse RCP calls it) is a name and the directories
// of its plugins. It either links libmyrcp and calls rcp::run() from its own main, or
// ships only a product directory that the `myrcp` launcher starts:
//
//   myrcp --product path/to/product        # reads path/to/product/product.toml
//
//   # product.toml
//   [product]
//   id = "songs"                 # its state: $XDG_STATE_HOME/<id>/ (perspectives, log)
//   name = "Songs"               # the workbench's title, --help and --version
//   version = "1.2.0"
//   plugins = ["plugins"]        # plugin directories, relative to the product directory
//                                # (each subdirectory with a myrcp.toml is a plugin);
//                                # default: the product directory itself
//   python = ".venv/bin/python"  # optional: the main plugin host's Python (a path with a
//                                # slash is relative to the product directory)
//
// Plugins are Python (the SDK, `import myrcp`), each run under the interpreter its
// manifest names; see ARCHITECTURE.md.
#pragma once

#include <string>
#include <vector>

#if defined(__GNUC__)
#define MYRCP_API __attribute__((visibility("default")))
#else
#define MYRCP_API
#endif

namespace rcp {

struct AppInfo {
  std::string id = "myrcp";    // state and log under $XDG_STATE_HOME/<id>/
  std::string name = "myrcp";  // what the workbench and its messages call itself
  std::string version;         // the application's; empty: myrcp's
  // Directories of plugins, searched after $MYRCP_PLUGIN_PATH.
  std::vector<std::string> plugin_dirs;
  // The Python running the plugin host; empty: $MYRCP_PYTHON, else python3.
  std::string python;
};

// The terminal workbench (libmyrcp). Parses the usual options (--list, --python,
// --vim-script, --version, --help) from argv and returns the exit status.
MYRCP_API int run(int argc, char** argv, const AppInfo& app);

// The GTK 4 workbench (libmyrcp-gtk, built with -DMYRCP_GTK=ON). The same options,
// but --list and --vim-script.
MYRCP_API int run_gtk(int argc, char** argv, const AppInfo& app);

// Reads <dir>/product.toml into `app` (a missing file: the directory's name as id and
// name, the directory as the plugin directory). Returns false with `error` when the
// file is broken.
MYRCP_API bool load_product(const std::string& dir, AppInfo& app, std::string& error);

// myrcp's own version.
MYRCP_API const char* version();

}  // namespace rcp
