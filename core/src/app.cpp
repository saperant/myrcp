#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <exception>
#include <poll.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "host.hpp"
#include "prefs.hpp"
#include "rcp/app.hpp"
#include "workbench.hpp"

namespace {

constexpr const char* kUsage = R"(usage: %s [options]

  --list           start the plugin host, print the contributions and exit
  --python PATH    the Python running the plugin host (default: $MYRCP_PYTHON or python3)
  --vim-script     print the script that links a Vim to myrcp (save it as
                   ~/.vim/plugin/myrcp.vim)
  --version        print the version
  --help           this text

Environment:
  MYRCP_PLUGIN_PATH   more directories of plugins (each a subdirectory with myrcp.toml)
  MYRCP_EDITOR_MODE   suspend (default) | tab | tmux-window | tmux-split
  MYRCP_NVIM_SERVER   the socket of a Neovim of your own, to link to
  MYRCP_VIM_SOCKET    where a Vim of your own links to (myrcp sets it for its terminals)
  VISUAL, EDITOR      the editor files open in
)";

// Initializes one host and returns its answer (nil on failure).
rcp::mp::Value initialize(const rcp::HostOptions& o) {
  using rcp::mp::Value;
  rcp::Host host(o);
  std::string err;
  if (!host.start(err)) {
    std::fprintf(stderr, "myrcp: %s\n", err.c_str());
    return Value();
  }
  Value result;
  bool done = false;
  host.rpc().request("initialize", rcp::mp::Map{{"protocol", 1}}, [&](const Value& e, const Value& res) {
    done = true;
    if (!e.is_nil()) std::fprintf(stderr, "myrcp: initialize failed: %s\n", std::string(e.str("error")).c_str());
    else result = res;
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!done && std::chrono::steady_clock::now() < deadline) {
    pollfd p{host.fd(), POLLIN, 0};
    if (poll(&p, 1, 100) > 0 && !host.rpc().on_readable()) {
      std::fprintf(stderr, "myrcp: the plugin host (%s) exited\n", o.python.c_str());
      host.exited();
      return Value();
    }
  }
  if (!done) std::fprintf(stderr, "myrcp: the plugin host (%s) did not answer\n", o.python.c_str());
  host.stop();
  return result;
}

// Headless: initialize the hosts and print what the plugins contribute.
int list_contributions(rcp::HostOptions o) {
  using rcp::mp::Value;
  o.log_path.clear();  // host errors straight to our stderr
  int rc = 0;
  std::vector<rcp::HostOptions> pending{o};
  for (size_t h = 0; h < pending.size(); ++h) {
    const Value res = initialize(pending[h]);
    if (res.is_nil()) {
      rc = 1;
      continue;
    }
    for (const auto& pl : res["plugins"].array()) {
      std::printf("plugin %s (%s)%s\n", std::string(pl["id"].str()).c_str(), std::string(pl["name"].str()).c_str(),
                  h > 0 ? (" [" + pending[h].python + "]").c_str() : "");
      for (const auto& v : pl["views"].array())
        std::printf("  view     %-28s %s%s\n", std::string(v["id"].str()).c_str(), std::string(v["title"].str()).c_str(),
                    v["startup"].boolean() ? " (startup)" : "");
      for (const auto& p : pl["preferences"].array())
        std::printf("  pref     %-28s %s (%s)\n", std::string(p["id"].str()).c_str(), std::string(p["title"].str()).c_str(),
                    std::string(p["type"].str()).c_str());
      for (const auto& p : pl["perspectives"].array())
        std::printf("  persp.   %-28s %s\n", std::string(p["id"].str()).c_str(), std::string(p["title"].str()).c_str());
      for (const auto& c : pl["commands"].array()) {
        std::string keys;
        for (const auto& k : c["keys"].array()) keys += (keys.empty() ? "" : ", ") + std::string(k.str());
        std::string extra;
        if (!keys.empty()) extra += "  [" + keys + "]";
        if (!c["view"].str().empty()) extra += "  in " + std::string(c["view"].str());
        std::printf("  command  %-28s %s%s\n", std::string(c["id"].str()).c_str(), std::string(c["title"].str()).c_str(),
                    extra.c_str());
      }
    }
    for (const auto& p : res["perspectives"].array())
      std::printf("perspective %s (%s, perspectives.toml)\n", std::string(p["id"].str()).c_str(),
                  std::string(p["title"].str()).c_str());
    for (const auto& e : res["errors"].array()) {
      std::printf("error: %s\n", std::string(e.str()).c_str());
      rc = std::max(rc, 2);
    }
    for (const auto& d : res["delegates"].array()) {
      rcp::HostOptions od = o;
      od.python = std::string(d["python"].str());
      od.only.clear();
      for (const auto& p : d["plugins"].array()) od.only += (od.only.empty() ? "" : ",") + std::string(p.str());
      pending.push_back(od);
    }
  }
  return rc;
}

// What the workbench starts from: the application's plugins, the SDK, its Python.
rcp::HostOptions options_for(const rcp::AppInfo& app) {
  rcp::HostOptions o;
  const char* py = std::getenv("MYRCP_PYTHON");
  o.python = !app.python.empty() ? app.python : py && *py ? py : "python3";
  o.python_dir = rcp::sdk_dir();
  for (const auto& d : app.plugin_dirs) o.plugins_dir += (o.plugins_dir.empty() ? "" : ":") + d;
  o.app_name = app.name.empty() ? "myrcp" : app.name;
  return o;
}

std::string version_of(const rcp::AppInfo& app) {
  return app.version.empty() ? std::string(MYRCP_VERSION) : app.version + " (myrcp " + MYRCP_VERSION + ")";
}

}  // namespace

namespace rcp {

const char* version() { return MYRCP_VERSION; }

bool load_product(const std::string& dir, AppInfo& app, std::string& error) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path root = fs::absolute(dir, ec).lexically_normal();
  std::string name = (root.filename().empty() ? root.parent_path() : root).filename().string();
  app.id = app.name = name;
  app.plugin_dirs = {root.string()};
  if (!fs::is_directory(root, ec)) {
    error = dir + ": not a directory";
    return false;
  }
  PrefsFile f;
  const fs::path toml = root / "product.toml";
  if (!fs::exists(toml, ec)) return true;
  if (!f.load(toml.string(), error)) {
    error = toml.string() + ": " + error;
    return false;
  }
  if (const auto& v = f.get("product.id"); v.is_str()) app.id = std::string(v.str());
  if (const auto& v = f.get("product.name"); v.is_str()) app.name = std::string(v.str());
  else if (f.get("product.id").is_str()) app.name = app.id;
  if (const auto& v = f.get("product.version"); v.is_str()) app.version = std::string(v.str());
  if (const auto& v = f.get("product.python"); v.is_str()) {
    const fs::path py(std::string(v.str()));
    app.python = py.is_relative() && std::string(v.str()).find('/') != std::string::npos ? (root / py).string() : py.string();
  }
  if (const auto& v = f.get("product.plugins"); v.is_array()) {
    app.plugin_dirs.clear();
    for (const auto& d : v.array()) {
      const fs::path p(std::string(d.str()));
      app.plugin_dirs.push_back((p.is_relative() ? root / p : p).lexically_normal().string());
    }
  }
  return true;
}

int run(int argc, char** argv, const AppInfo& app) {
  std::signal(SIGPIPE, SIG_IGN);
  HostOptions o = options_for(app);
  const char* me = o.app_name.c_str();
  bool list = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      std::printf(kUsage, me);
      return 0;
    } else if (a == "--version") {
      std::printf("%s %s\n", me, version_of(app).c_str());
      return 0;
    } else if (a == "--vim-script") {
      std::fputs(vim_script(), stdout);
      return 0;
    } else if (a == "--list") {
      list = true;
    } else if (a == "--python" && i + 1 < argc) {
      o.python = argv[++i];
    } else {
      std::fprintf(stderr, "%s: unknown option %s\n", me, a.c_str());
      std::fprintf(stderr, kUsage, me);
      return 2;
    }
  }
  if (list) return list_contributions(o);
  if (!isatty(0) || !isatty(1)) {
    std::fprintf(stderr, "%s: needs a terminal (try --list)\n", me);
    return 1;
  }
  o.log_path = default_log_path(app.id);
  o.state_path = default_state_path(app.id);
  o.prefs_path = default_prefs_path();
  try {
    Workbench wb(o);
    return wb.run();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "\x1b[?1049l\x1b[?25h%s: %s\n", me, e.what());
    return 1;
  }
}

}  // namespace rcp
