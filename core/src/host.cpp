#include "host.hpp"

#include <cstdlib>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>

namespace rcp {

namespace {

std::string env_or(const char* name, const std::string& def) {
  const char* v = std::getenv(name);
  return v && *v ? std::string(v) : def;
}

}  // namespace

namespace {

std::filesystem::path state_dir(const std::string& app) {
  namespace fs = std::filesystem;
  fs::path base = env_or("XDG_STATE_HOME", "");
  if (base.empty()) base = fs::path(env_or("HOME", "/tmp")) / ".local/state";
  const fs::path dir = base / (app.empty() ? "myrcp" : app);
  std::error_code ec;
  fs::create_directories(dir, ec);
  return dir;
}

}  // namespace

std::string sdk_dir() {
  if (const char* e = std::getenv("MYRCP_SDK"); e && *e) return e;
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(&sdk_dir), &info) && info.dli_fname) {
    std::error_code ec;
    const auto self = std::filesystem::canonical(info.dli_fname, ec);
    const auto share = self.parent_path().parent_path() / "share/myrcp/python";
    if (!ec && std::filesystem::exists(share / "myrcp/host.py", ec)) return share.string();
  }
#ifdef MYRCP_PYTHON_DIR
  return MYRCP_PYTHON_DIR;
#else
  return "";
#endif
}

std::string default_log_path(const std::string& app) { return (state_dir(app) / "host.log").string(); }

std::string default_state_path(const std::string& app) { return (state_dir(app) / "workbench.state").string(); }

std::string default_prefs_path() {
  namespace fs = std::filesystem;
  fs::path base = env_or("XDG_CONFIG_HOME", "");
  if (base.empty()) base = fs::path(env_or("HOME", "/tmp")) / ".config";
  return (base / "myrcp" / "preferences.toml").string();
}

bool Host::start(std::string& error) {
  stop();
  Env env;
  std::string pythonpath = o_.python_dir;
  if (const char* p = std::getenv("PYTHONPATH"); p && *p) pythonpath += std::string(":") + p;
  env.emplace_back("PYTHONPATH", pythonpath);
  std::string plugin_path = env_or("MYRCP_PLUGIN_PATH", "");
  if (!o_.plugins_dir.empty()) plugin_path += (plugin_path.empty() ? "" : ":") + o_.plugins_dir;
  env.emplace_back("MYRCP_PLUGIN_PATH", plugin_path);
  env.emplace_back("MYRCP_HOST_PLUGINS", o_.only);

  const int log = o_.log_path.empty() ? -1 : open(o_.log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  child_ = spawn_piped({o_.python, "-m", "myrcp.host"}, env, log);
  if (log >= 0) close(log);
  if (child_.pid <= 0) {
    error = "cannot start the plugin host (" + o_.python + ")";
    return false;
  }
  rpc_ = std::make_unique<Rpc>(child_.from_child, child_.to_child);
  return true;
}

void Host::stop() {
  if (child_.pid <= 0) return;
  if (rpc_) rpc_->notify("shutdown", mp::Map{});
  if (child_.to_child >= 0) {
    close(child_.to_child);  // EOF ends the host's read loop too
    child_.to_child = -1;
  }
  reap(child_.pid, 500);
  child_.pid = -1;
  close_fds();
}

void Host::exited() {
  if (child_.pid > 0) reap(child_.pid, 200);
  child_.pid = -1;
  close_fds();
}

void Host::close_fds() {
  if (rpc_) rpc_->fail_pending("plugin host exited");
  if (child_.to_child >= 0) close(child_.to_child);
  if (child_.from_child >= 0) close(child_.from_child);
  child_.to_child = child_.from_child = -1;
}

}  // namespace rcp
