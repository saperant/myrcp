// A plugin host process (python -m myrcp.host) and its RPC connection.
#pragma once

#include <memory>
#include <string>

#include "process.hpp"
#include "rpc.hpp"

namespace rcp {

struct HostOptions {
  std::string python = "python3";
  std::string python_dir;   // prepended to PYTHONPATH (the myrcp SDK)
  std::string plugins_dir;  // appended to MYRCP_PLUGIN_PATH (bundled plugins)
  std::string log_path;     // the host's stderr
  std::string only;         // comma-separated plugin ids: a host serving just these
  std::string state_path;   // where the hub keeps the perspectives; empty: nowhere
  std::string prefs_path;   // the preferences file; empty: none (defaults and env only)
  std::string app_name = "myrcp";  // what the workbench calls itself (rcp::AppInfo::name)
};

class Host {
 public:
  explicit Host(HostOptions o) : o_(std::move(o)) {}
  ~Host() { stop(); }
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;

  bool start(std::string& error);
  // Asks the host to shut down, then kills it if it lingers.
  void stop();
  // Reaps a host whose pipe reached EOF.
  void exited();
  bool running() const { return child_.pid > 0; }
  int fd() const { return child_.from_child; }
  Rpc& rpc() { return *rpc_; }
  const HostOptions& options() const { return o_; }

 private:
  void close_fds();
  HostOptions o_;
  Child child_;
  std::unique_ptr<Rpc> rpc_;
};

// The SDK of an installed myrcp: <prefix>/share/myrcp/python, next to the <prefix>/lib
// (or <prefix>/bin) this code was loaded from. Without it (a build tree), the
// source tree's (MYRCP_PYTHON_DIR); $MYRCP_SDK wins over both.
std::string sdk_dir();

// $XDG_STATE_HOME/<app>/host.log (created).
std::string default_log_path(const std::string& app = "myrcp");
// $XDG_STATE_HOME/<app>/workbench.state
std::string default_state_path(const std::string& app = "myrcp");
// $XDG_CONFIG_HOME/myrcp/preferences.toml (not created: only a change writes it)
std::string default_prefs_path();

}  // namespace rcp
