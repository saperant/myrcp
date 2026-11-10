// The Vim link: Vim (+channel, +timers) connects to a unix socket of ours in "json" mode
// and runs vim_script(); we call its functions with ["call", name, args] messages and
// it sends us [n, {kind: …}] ones, a line each.
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "editor_link.hpp"
#include "json.hpp"

namespace rcp {

using mp::Array;
using mp::Map;
using mp::Value;

namespace {

constexpr const char* kScript = R"vim(" myrcp.vim: links this Vim to myrcp, both ways. myrcp's problems show here as
" signs (and as text after the line); files and problems opened in myrcp open here; where
" the cursor rests is myrcp's selection (Properties), and the quickfix list (:make,
" :grep) is in myrcp's Problems.
"
" Vims that myrcp starts load it themselves. For a Vim of your own, save this file as
" ~/.vim/plugin/myrcp.vim (myrcp --vim-script > ~/.vim/plugin/myrcp.vim): it links to
" the myrcp that runs, whenever one does. The socket is $MYRCP_VIM_SOCKET, else
" $XDG_RUNTIME_DIR/myrcp-vim.sock (myrcp's workbench.vim_socket preference).
if exists('g:myrcp_loaded') || !has('channel') || !has('timers') || !has('signs')
  finish
endif
let g:myrcp_loaded = 1

let s:ch = v:none
let s:last = ''
let s:sent = ''
let s:markers = {}
let s:timer = -1
let s:types = {'error': ['MyrcpError', 'ErrorMsg', 'E>'], 'warning': ['MyrcpWarning', 'WarningMsg', 'W>'],
      \ 'info': ['MyrcpInfo', 'Question', 'I>']}
let s:vtext = has('textprop') && has('patch-9.0.0067')
for s:t in values(s:types)
  call sign_define(s:t[0], {'text': s:t[2], 'texthl': s:t[1]})
  if s:vtext && empty(prop_type_get(s:t[0]))
    call prop_type_add(s:t[0], {'highlight': s:t[1]})
  endif
endfor

function! s:address() abort
  if exists('g:myrcp_socket')
    return g:myrcp_socket
  elseif $MYRCP_VIM_SOCKET !=# ''
    return $MYRCP_VIM_SOCKET
  elseif $XDG_RUNTIME_DIR !=# ''
    return $XDG_RUNTIME_DIR . '/myrcp-vim.sock'
  endif
  return '/tmp/myrcp-vim-' . $USER . '.sock'
endfunction

function! s:open() abort
  return type(s:ch) == v:t_channel && ch_status(s:ch) ==# 'open'
endfunction

function! s:send(msg) abort
  if s:open()
    call ch_sendexpr(s:ch, a:msg)
  endif
endfunction

" Tried now and every two seconds: myrcp may start after this Vim, or again.
function! s:connect(...) abort
  if s:open()
    return
  endif
  let addr = s:address()
  if getftype(addr) !=# 'socket'
    return
  endif
  try
    let s:ch = ch_open('unix:' . addr, {'mode': 'json',
          \ 'close_cb': function('s:closed')})
  catch
    let s:ch = v:none
  endtry
  if !s:open()
    return
  endif
  let s:last = ''
  let s:sent = ''
  call s:send({'kind': 'hello', 'version': v:version})
  call s:cursor()
  call s:quickfix()
endfunction

function! s:closed(ch) abort
  let s:ch = v:none
  call MyrcpMarkers({})  " myrcp's problems go with it
endfunction

function! s:apply(buf) abort
  if !bufexists(a:buf) || !bufloaded(a:buf)
    return
  endif
  call sign_unplace('myrcp', {'buffer': a:buf})
  if s:vtext
    for t in values(s:types)
      silent! call prop_remove({'type': t[0], 'bufnr': a:buf, 'all': 1})
    endfor
  endif
  let name = bufname(a:buf)
  if name ==# '' || getbufvar(a:buf, '&buftype') !=# ''
    return
  endif
  let lines = getbufinfo(a:buf)[0].linecount
  for m in get(s:markers, resolve(fnamemodify(name, ':p')), [])
    if m.line < 1 || m.line > lines
      continue
    endif
    let t = get(s:types, m.severity, s:types.info)
    call sign_place(0, 'myrcp', t[0], a:buf, {'lnum': m.line})
    if s:vtext
      silent! call prop_add(m.line, 0, {'type': t[0], 'bufnr': a:buf, 'text': '  ' . m.source . ': ' . m.message,
            \ 'text_align': 'after'})
    endif
  endfor
endfunction

" myrcp's problems, {path: [{line, col, severity, message, source}]}.
function! MyrcpMarkers(markers) abort
  let s:markers = a:markers
  for b in getbufinfo({'bufloaded': 1})
    call s:apply(b.bufnr)
  endfor
  redraw
endfunction

function! MyrcpGoto(path, line) abort
  if resolve(expand('%:p')) !=# resolve(fnamemodify(a:path, ':p'))
    " Its window if it has one; else here, or a split when this buffer cannot be left.
    execute 'drop' fnameescape(a:path)
  endif
  if a:line > 0
    call cursor(a:line, 1)
    normal! zz
  endif
  redraw
  call s:later()  " no CursorMoved for a jump from a channel
endfunction

function! s:cursor(...) abort
  let s:timer = -1
  if !s:open() || &buftype !=# '' || expand('%') ==# ''
    return
  endif
  let path = resolve(expand('%:p'))
  let key = path . ':' . line('.')
  if key ==# s:last
    return
  endif
  let s:last = key
  call s:send({'kind': 'cursor', 'path': path, 'line': line('.'), 'col': col('.'),
        \ 'modified': &modified ? v:true : v:false, 'filetype': &filetype, 'text': getline('.')})
endfunction

function! s:later() abort
  if s:timer >= 0
    call timer_stop(s:timer)
  endif
  let s:timer = timer_start(150, function('s:cursor'))
endfunction

" The quickfix list is this Vim's own diagnostics.
function! s:quickfix() abort
  let out = []
  for e in getqflist()
    if e.valid && e.bufnr > 0 && e.lnum > 0
      let sev = e.type =~? '^w' ? 'warning' : e.type =~? '^[in]' ? 'info' : 'error'
      call add(out, {'path': resolve(fnamemodify(bufname(e.bufnr), ':p')), 'line': e.lnum, 'col': e.col,
            \ 'message': trim(e.text), 'source': 'quickfix', 'severity': sev})
    endif
  endfor
  let key = string(out)
  if key ==# s:sent || !s:open()
    return
  endif
  let s:sent = key
  call s:send({'kind': 'diagnostics', 'items': out})
endfunction

augroup myrcp_link
  autocmd!
  autocmd CursorMoved,CursorMovedI,BufEnter,FocusGained * call s:later()
  autocmd BufReadPost * call s:apply(str2nr(expand('<abuf>')))
  autocmd QuickFixCmdPost * call s:quickfix()
augroup END

command! -nargs=? MyrcpLink if <q-args> !=# '' | let g:myrcp_socket = <q-args> | endif | call s:connect()
call s:connect()
call timer_start(2000, function('s:connect'), {'repeat': -1})
)vim";

int unix_listen(const std::string& path) {
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof addr.sun_path) {
    close(fd);
    return -1;
  }
  std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(fd, 4) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

bool someone_listens(const std::string& path) {
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
  const bool yes = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
  close(fd);
  return yes;
}

}  // namespace

const char* vim_script() { return kScript; }

std::string default_vim_socket() {
  if (const char* run = std::getenv("XDG_RUNTIME_DIR"); run && *run) return std::string(run) + "/myrcp-vim.sock";
  const char* user = std::getenv("USER");
  return "/tmp/myrcp-vim-" + std::string(user ? user : "") + ".sock";
}

VimLink::VimLink(std::string sock) : EditorLink(sock, true), sock_(std::move(sock)) {
  // Listening before Vim starts: it connects as soon as the script runs.
  std::error_code ec;
  std::filesystem::remove(sock_, ec);
  listen_ = unix_listen(sock_);
}

VimLink::VimLink(int conn, std::string key) : EditorLink(std::move(key), false), conn_(conn) {}

VimLink::~VimLink() {
  if (conn_ >= 0) close(conn_);
  if (listen_ >= 0) close(listen_);
  if (!sock_.empty()) {
    std::error_code ec;
    std::filesystem::remove(sock_, ec);
  }
}

bool VimLink::connect() {
  if (conn_ >= 0) return true;
  if (listen_ < 0) return false;
  const int c = accept4(listen_, nullptr, nullptr, SOCK_CLOEXEC);
  if (c < 0) return false;
  conn_ = c;
  close(listen_);  // one Vim per socket of ours
  listen_ = -1;
  push_markers(markers_);
  if (on_connected) on_connected();
  return true;
}

void VimLink::send(const Value& msg) {
  if (conn_ < 0) return;
  const std::string line = to_json(msg) + "\n";
  std::string_view s = line;
  while (!s.empty()) {
    const ssize_t n = ::send(conn_, s.data(), s.size(), MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return;  // gone: the reader sees EOF next
    }
    s.remove_prefix(size_t(n));
  }
}

bool VimLink::on_readable() {
  if (conn_ < 0) return false;
  char buf[8192];
  const ssize_t n = ::read(conn_, buf, sizeof buf);
  if (n < 0) return errno == EINTR || errno == EAGAIN;
  if (n == 0) return false;
  in_.append(buf, size_t(n));
  size_t start = 0;
  for (size_t nl; (nl = in_.find('\n', start)) != std::string::npos; start = nl + 1) {
    // [n, message]: what ch_sendexpr() sends. Anything else is ignored.
    const auto v = from_json(std::string_view(in_).substr(start, nl - start));
    if (v && v->is_array() && v->array().size() == 2) message(v->at(1));
  }
  in_.erase(0, start);
  if (in_.size() > (16u << 20)) return false;  // no line in sight: not a Vim of ours
  return true;
}

void VimLink::message(const Value& m) {
  const std::string_view kind = m["kind"].str();
  if (kind == "cursor" && on_cursor) {
    on_cursor(EditorCursor{std::string(m["path"].str()), std::string(m["filetype"].str()),
                           std::string(m["text"].str()), int(m["line"].integer()), int(m["col"].integer()),
                           m["modified"].boolean()});
  } else if (kind == "diagnostics" && on_diagnostics) {
    std::vector<Marker> out;
    if (m["items"].is_array()) {
      for (const auto& d : m["items"].array()) {
        out.push_back({std::string(d["path"].str()), int(d["line"].integer()), int(d["col"].integer()),
                       std::string(d["severity"].str("error")), std::string(d["message"].str()),
                       std::string(d["source"].str("quickfix")), ""});
      }
    }
    on_diagnostics(std::move(out));
  }
}

void VimLink::push_markers(const std::vector<Marker>& markers) {
  markers_ = markers;
  if (conn_ < 0) return;
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
  send(Array{"call", "MyrcpMarkers", Array{std::move(table)}});
}

void VimLink::go_to(const std::string& path, int line) { send(Array{"call", "MyrcpGoto", Array{path, line}}); }

namespace {
std::string fnameescape(const std::string& path) {  // as Vim's fnameescape() does
  std::string out;
  for (size_t i = 0; i < path.size(); ++i) {
    const char c = path[i];
    if (std::string_view(" \t\n*?[{`$\\%#'\"|!<").find(c) != std::string_view::npos || (i == 0 && (c == '-' || c == '+')))
      out += '\\';
    out += c;
  }
  return out;
}
}  // namespace

void VimLink::diff(const std::string& left, const std::string& right) {
  send(Array{"ex", "tabedit " + fnameescape(left) + " | diffthis | vertical diffsplit " + fnameescape(right)});
}

VimListener::~VimListener() { close(); }

bool VimListener::listen(const std::string& path) {
  close();
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    if (someone_listens(path)) return false;  // another myrcp's
    std::filesystem::remove(path, ec);        // a stale one
  }
  fd_ = unix_listen(path);
  if (fd_ < 0) return false;
  path_ = path;
  return true;
}

void VimListener::close() {
  if (fd_ < 0) return;
  ::close(fd_);
  fd_ = -1;
  std::error_code ec;
  std::filesystem::remove(path_, ec);
  path_.clear();
}

std::unique_ptr<VimLink> VimListener::accept() {
  if (fd_ < 0) return nullptr;
  const int c = accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
  if (c < 0) return nullptr;
  return std::make_unique<VimLink>(c, path_ + "#" + std::to_string(next_++));
}

}  // namespace rcp
