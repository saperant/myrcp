#include "editor_link.hpp"

#include <filesystem>
#include <map>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace rcp {

using mp::Array;
using mp::Map;
using mp::Value;

namespace {

// Neovim's diagnostics from our markers: set for every loaded buffer, and for each one
// read later. Paths are compared resolved (symlinks such as /home → /var/home).
constexpr const char* kMarkers = R"lua(
local markers = ...
_G.myrcp = _G.myrcp or {}
myrcp.markers = markers
local ns = vim.api.nvim_create_namespace('myrcp')
local sev = { error = vim.diagnostic.severity.ERROR, warning = vim.diagnostic.severity.WARN,
              info = vim.diagnostic.severity.INFO }
local function apply(buf)
  local name = vim.fn.resolve(vim.api.nvim_buf_get_name(buf))
  local items = {}
  for _, m in ipairs(myrcp.markers[name] or {}) do
    table.insert(items, { lnum = m.line - 1, col = m.col, message = m.message, source = m.source,
                          severity = sev[m.severity] or vim.diagnostic.severity.INFO })
  end
  vim.diagnostic.set(ns, buf, items)
end
for _, b in ipairs(vim.api.nvim_list_bufs()) do
  if vim.api.nvim_buf_is_loaded(b) then apply(b) end
end
if not myrcp.au then
  myrcp.au = vim.api.nvim_create_autocmd('BufReadPost', { callback = function(a) apply(a.buf) end })
end
)lua";

constexpr const char* kGoto = R"lua(
local path, line = ...
if vim.fn.resolve(vim.api.nvim_buf_get_name(0)) ~= vim.fn.resolve(path) then
  vim.cmd('edit ' .. vim.fn.fnameescape(path))
end
if line > 0 then pcall(vim.api.nvim_win_set_cursor, 0, { line, 0 }) end
)lua";

// What Neovim tells us, on our channel: where the cursor rests (once it has rested a
// moment, and only when it is on another line or file) and its own diagnostics (all
// but ours, whenever they change).
constexpr const char* kLink = R"lua(
local chan = ...
_G.myrcp = _G.myrcp or {}
myrcp.chan = chan
myrcp.last = nil
myrcp.sent = nil
local uv = vim.uv or vim.loop
local group = vim.api.nvim_create_augroup('myrcp_link', { clear = true })
local function cursor()
  local buf = vim.api.nvim_get_current_buf()
  local name = vim.api.nvim_buf_get_name(buf)
  if name == '' or vim.bo[buf].buftype ~= '' then return end
  local pos = vim.api.nvim_win_get_cursor(0)
  local path = vim.fn.resolve(name)
  local key = path .. ':' .. pos[1]
  if key == myrcp.last then return end
  myrcp.last = key
  vim.rpcnotify(myrcp.chan, 'myrcp_cursor', { path = path, line = pos[1], col = pos[2] + 1,
    modified = vim.bo[buf].modified, filetype = vim.bo[buf].filetype,
    text = vim.api.nvim_get_current_line() })
end
local ours = vim.api.nvim_create_namespace('myrcp')
local names = { 'error', 'warning', 'info', 'info' }
local function diagnostics()
  local out = {}
  for _, d in ipairs(vim.diagnostic.get(nil)) do
    if d.namespace ~= ours and vim.api.nvim_buf_is_valid(d.bufnr) then
      local name = vim.api.nvim_buf_get_name(d.bufnr)
      if name ~= '' then
        table.insert(out, { path = vim.fn.resolve(name), line = d.lnum + 1, col = d.col + 1,
          message = d.message, source = d.source or 'nvim', severity = names[d.severity] or 'info' })
      end
    end
  end
  local key = vim.inspect(out)
  if key == myrcp.sent then return end  -- ours being set changes nothing here
  myrcp.sent = key
  vim.rpcnotify(myrcp.chan, 'myrcp_diagnostics', out)
end
local function later(ms, fn)
  local t = uv.new_timer()
  return function()
    t:stop()
    t:start(ms, 0, vim.schedule_wrap(fn))
  end
end
vim.api.nvim_create_autocmd({ 'CursorMoved', 'CursorMovedI', 'BufEnter', 'FocusGained' },
  { group = group, callback = later(150, cursor) })
vim.api.nvim_create_autocmd('DiagnosticChanged', { group = group, callback = later(250, diagnostics) })
cursor()
diagnostics()
)lua";

}  // namespace

EditorKind editor_kind(const std::string& editor) {
  const std::string first = editor.substr(0, editor.find(' '));
  const std::string name = std::filesystem::path(first).filename().string();
  if (name.starts_with("nvim")) return EditorKind::nvim;
  if (name.starts_with("vim")) return EditorKind::vim;
  return EditorKind::other;
}

NvimLink::NvimLink(std::string sock, bool owned) : EditorLink(std::move(sock), owned) {}

NvimLink::~NvimLink() {
  rpc_.reset();
  if (fd_ >= 0) close(fd_);
  if (owned_ && !key_.empty()) {
    std::error_code ec;
    std::filesystem::remove(key_, ec);
  }
}

bool NvimLink::connect() {
  if (rpc_) return true;
  std::error_code ec;
  if (!std::filesystem::exists(key_, ec)) return false;
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", key_.c_str());
  if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    if (fd >= 0) close(fd);
    return false;
  }
  fd_ = fd;
  rpc_ = std::make_unique<Rpc>(fd, fd);
  rpc_->on_notify = [this](std::string_view method, const Value& p) {
    if (method == "myrcp_cursor" && on_cursor) {
      on_cursor(EditorCursor{std::string(p["path"].str()), std::string(p["filetype"].str()), std::string(p["text"].str()),
                 int(p["line"].integer()), int(p["col"].integer()), p["modified"].boolean()});
    } else if (method == "myrcp_diagnostics" && on_diagnostics) {
      std::vector<Marker> out;
      if (p.is_array()) {
        for (const auto& d : p.array()) {
          out.push_back({std::string(d["path"].str()), int(d["line"].integer()), int(d["col"].integer()),
                         std::string(d["severity"].str("info")), std::string(d["message"].str()),
                         std::string(d["source"].str("nvim")), ""});
        }
      }
      on_diagnostics(std::move(out));
    }
  };
  // Our channel id, for Neovim to notify us on.
  rpc_->request_args("nvim_get_api_info", Array{}, [this](const Value& err, const Value& res) {
    if (!err.is_nil() || !rpc_) return;
    rpc_->notify_args("nvim_exec_lua", Array{kLink, Array{res.at(0)}});
    if (on_connected) on_connected();
  });
  push_markers(markers_);
  return true;
}

bool NvimLink::on_readable() { return rpc_ && rpc_->on_readable(); }

void NvimLink::push_markers(const std::vector<Marker>& markers) {
  markers_ = markers;
  if (!rpc_) return;
  std::map<std::string, Array> by_path;
  for (const auto& m : markers) {
    if (m.path.empty() || m.line <= 0) continue;
    std::error_code ec;
    const auto resolved = std::filesystem::weakly_canonical(m.path, ec).string();
    by_path[ec ? m.path : resolved].push_back(Map{{"line", m.line}, {"col", m.col}, {"severity", m.severity},
                                                   {"message", m.message}, {"source", m.source}});
  }
  Map table;
  for (auto& [path, items] : by_path) table.emplace_back(path, std::move(items));
  rpc_->notify_args("nvim_exec_lua", Array{kMarkers, Array{std::move(table)}});
}

void NvimLink::go_to(const std::string& path, int line) {
  if (rpc_) rpc_->notify_args("nvim_exec_lua", Array{kGoto, Array{path, line}});
}

void NvimLink::diff(const std::string& left, const std::string& right) {
  static constexpr const char* kDiff = R"lua(
local l, r = ...
vim.cmd('tabedit ' .. vim.fn.fnameescape(l))
vim.cmd('diffthis')
vim.cmd('vertical diffsplit ' .. vim.fn.fnameescape(r))
)lua";
  if (rpc_) rpc_->notify_args("nvim_exec_lua", Array{kDiff, Array{left, right}});
}

}  // namespace rcp
