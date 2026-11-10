// msgpack-RPC over a pair of fds: [0,id,method,params] [1,id,error,result] [2,method,params].
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

#include "rcp/msgpack.hpp"

namespace rcp {

class Rpc {
 public:
  using Callback = std::function<void(const mp::Value& error, const mp::Value& result)>;
  using NotifyHandler = std::function<void(std::string_view method, const mp::Value& param)>;
  using RequestHandler = std::function<mp::Value(std::string_view method, const mp::Value& param)>;

  Rpc(int read_fd, int write_fd) : read_fd_(read_fd), write_fd_(write_fd) {}

  // Params are always one map (sent as a one-element array).
  void notify(std::string_view method, mp::Value param);
  // With the arguments as they are (Neovim's API takes positional ones).
  void notify_args(std::string_view method, mp::Array args);
  void request(std::string_view method, mp::Value param, Callback cb);
  void request_args(std::string_view method, mp::Array args, Callback cb);
  // Call when read_fd is readable. Returns false on EOF or a protocol error.
  bool on_readable();
  // Fails outstanding requests (the peer is gone).
  void fail_pending(std::string_view why);
  bool broken() const { return broken_; }

  NotifyHandler on_notify;
  RequestHandler on_request;  // throw std::exception to answer with an error
  // Tried first: return true to answer later with respond(msgid, ...).
  std::function<bool(std::string_view method, const mp::Value& param, const mp::Value& msgid)> on_request_later;
  void respond(const mp::Value& msgid, mp::Value error, mp::Value result);

 private:
  void send(const mp::Value& msg);
  void dispatch(const mp::Value& msg);

  int read_fd_, write_fd_;
  uint32_t next_id_ = 1;
  std::map<uint32_t, Callback> pending_;
  mp::Decoder decoder_;
  bool broken_ = false;
};

}  // namespace rcp
