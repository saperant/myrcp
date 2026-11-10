// rcp::run_gtk (libmyrcp-gtk): the workbench as a GTK 4 application, over the same
// plugin hosts and views as the terminal workbench (core/src/hub.hpp).
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#include "host.hpp"
#include "rcp/app.hpp"
#include "workbench.hpp"

namespace {

constexpr const char* kUsage = R"(usage: %s [options]

  --python PATH    the Python running the plugin host (default: $MYRCP_PYTHON or python3)
  --version        print the version
  --help           this text

Environment:
  MYRCP_PLUGIN_PATH   more directories of plugins (each a subdirectory with myrcp.toml)
  MYRCP_NVIM_SERVER   the socket of a Neovim of your own, to link to
  VISUAL, EDITOR      the editor files open in (in a terminal tab)
)";

}  // namespace

namespace rcp {

int run_gtk(int argc, char** argv, const AppInfo& app) {
  std::signal(SIGPIPE, SIG_IGN);
  HostOptions o;
  const char* py = std::getenv("MYRCP_PYTHON");
  o.python = !app.python.empty() ? app.python : py && *py ? py : "python3";
  o.python_dir = sdk_dir();
  for (const auto& d : app.plugin_dirs) o.plugins_dir += (o.plugins_dir.empty() ? "" : ":") + d;
  o.app_name = app.name.empty() ? "myrcp" : app.name;
  const char* me = o.app_name.c_str();
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      std::printf(kUsage, me);
      return 0;
    } else if (a == "--version") {
      const std::string v = app.version.empty() ? version() : app.version + " (myrcp " + version() + ")";
      std::printf("%s %s\n", me, v.c_str());
      return 0;
    } else if (a == "--python" && i + 1 < argc) {
      o.python = argv[++i];
    } else {
      std::fprintf(stderr, "%s: unknown option %s\n", me, a.c_str());
      std::fprintf(stderr, kUsage, me);
      return 2;
    }
  }
  o.log_path = default_log_path(app.id);
  o.state_path = default_state_path(app.id);
  o.prefs_path = default_prefs_path();
  try {
    gtk::Workbench wb(o);
    return wb.run(argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", me, e.what());
    return 1;
  }
}

}  // namespace rcp
