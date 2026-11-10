#include "rpc.hpp"

#include <cerrno>
#include <unistd.h>

namespace rcp {

using mp::Array;
using mp::Value;

void Rpc::send(const Value& msg) {
  if (broken_) return;
  const std::string data = mp::encode(msg);
  std::string_view s = data;
  while (!s.empty()) {
    const ssize_t n = ::write(write_fd_, s.data(), s.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      broken_ = true;  // EPIPE: the reader sees EOF next
      return;
    }
    s.remove_prefix(size_t(n));
  }
}

void Rpc::notify(std::string_view method, Value param) {
  send(Array{2, method, Array{std::move(param)}});
}

void Rpc::notify_args(std::string_view method, Array args) { send(Array{2, method, std::move(args)}); }

void Rpc::request(std::string_view method, Value param, Callback cb) { request_args(method, Array{std::move(param)}, std::move(cb)); }

void Rpc::request_args(std::string_view method, Array args, Callback cb) {
  const uint32_t id = next_id_++;
  if (broken_) {
    if (cb) cb(Value("peer is gone"), Value());
    return;
  }
  pending_[id] = std::move(cb);
  send(Array{0, id, method, std::move(args)});
}

bool Rpc::on_readable() {
  char buf[65536];
  ssize_t n;
  do {
    n = ::read(read_fd_, buf, sizeof buf);
  } while (n < 0 && errno == EINTR);
  if (n <= 0) {
    broken_ = true;
    return false;
  }
  decoder_.feed({buf, size_t(n)});
  try {
    while (auto msg = decoder_.next()) dispatch(*msg);
  } catch (const mp::DecodeError&) {
    broken_ = true;
    return false;
  }
  return true;
}

void Rpc::dispatch(const Value& msg) {
  switch (msg.at(0).integer(-1)) {
    case 0: {
      const Value& id = msg.at(1);
      if (on_request_later && on_request_later(msg.at(2).str(), msg.at(3).at(0), id)) break;
      Value error, result;
      try {
        if (!on_request) throw std::runtime_error("no request handler");
        result = on_request(msg.at(2).str(), msg.at(3).at(0));
      } catch (const std::exception& e) {
        error = Value(e.what());
      }
      send(Array{1, id, std::move(error), std::move(result)});
      break;
    }
    case 1: {
      const auto it = pending_.find(uint32_t(msg.at(1).integer()));
      if (it == pending_.end()) break;
      Callback cb = std::move(it->second);
      pending_.erase(it);
      if (cb) cb(msg.at(2), msg.at(3));
      break;
    }
    case 2:
      if (on_notify) on_notify(msg.at(1).str(), msg.at(2).at(0));
      break;
    default:
      break;
  }
}

void Rpc::respond(const Value& msgid, Value error, Value result) {
  send(Array{1, msgid, std::move(error), std::move(result)});
}

void Rpc::fail_pending(std::string_view why) {
  auto pending = std::move(pending_);
  pending_.clear();
  for (auto& [id, cb] : pending) {
    if (cb) cb(Value(why), Value());
  }
}

}  // namespace rcp
