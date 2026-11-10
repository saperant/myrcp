#include "rcp/msgpack.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

namespace rcp::mp {

// ---- Value accessors --------------------------------------------------------------

const Value& Value::nil() {
  static const Value n;
  return n;
}

std::string_view Value::str(std::string_view def) const {
  if (auto p = std::get_if<std::string>(&v)) return *p;
  return def;
}

int64_t Value::integer(int64_t def) const {
  if (auto p = std::get_if<int64_t>(&v)) return *p;
  if (auto p = std::get_if<uint64_t>(&v)) {
    return *p > uint64_t(std::numeric_limits<int64_t>::max()) ? std::numeric_limits<int64_t>::max()
                                                              : int64_t(*p);
  }
  if (auto p = std::get_if<double>(&v)) return int64_t(*p);
  return def;
}

double Value::number(double def) const {
  if (auto p = std::get_if<double>(&v)) return *p;
  if (auto p = std::get_if<int64_t>(&v)) return double(*p);
  if (auto p = std::get_if<uint64_t>(&v)) return double(*p);
  return def;
}

bool Value::boolean(bool def) const {
  if (auto p = std::get_if<bool>(&v)) return *p;
  return def;
}

const Array& Value::array() const {
  static const Array empty;
  if (auto p = std::get_if<Array>(&v)) return *p;
  return empty;
}

const Map& Value::map() const {
  static const Map empty;
  if (auto p = std::get_if<Map>(&v)) return *p;
  return empty;
}

const Value& Value::operator[](std::string_view key) const {
  for (const auto& [k, val] : map()) {
    if (auto s = std::get_if<std::string>(&k.v); s && *s == key) return val;
  }
  return nil();
}

const Value& Value::at(size_t i) const {
  const auto& a = array();
  return i < a.size() ? a[i] : nil();
}

bool operator==(const Value& a, const Value& b) { return a.v == b.v; }

// ---- Encoder ------------------------------------------------------------------------

namespace {

void put_be(std::string& out, uint64_t v, int bytes) {
  for (int i = bytes - 1; i >= 0; --i) out.push_back(char((v >> (8 * i)) & 0xff));
}

void put_uint(std::string& out, uint64_t u) {
  if (u < 0x80) {
    out.push_back(char(u));
  } else if (u <= 0xff) {
    out.push_back(char(0xcc));
    put_be(out, u, 1);
  } else if (u <= 0xffff) {
    out.push_back(char(0xcd));
    put_be(out, u, 2);
  } else if (u <= 0xffffffff) {
    out.push_back(char(0xce));
    put_be(out, u, 4);
  } else {
    out.push_back(char(0xcf));
    put_be(out, u, 8);
  }
}

void put_int(std::string& out, int64_t i) {
  if (i >= 0) return put_uint(out, uint64_t(i));
  if (i >= -32) {
    out.push_back(char(int8_t(i)));
  } else if (i >= std::numeric_limits<int8_t>::min()) {
    out.push_back(char(0xd0));
    put_be(out, uint64_t(i), 1);
  } else if (i >= std::numeric_limits<int16_t>::min()) {
    out.push_back(char(0xd1));
    put_be(out, uint64_t(i), 2);
  } else if (i >= std::numeric_limits<int32_t>::min()) {
    out.push_back(char(0xd2));
    put_be(out, uint64_t(i), 4);
  } else {
    out.push_back(char(0xd3));
    put_be(out, uint64_t(i), 8);
  }
}

// Header for str/bin/array/map: fix form when it fits, else 8/16/32-bit length.
void put_header(std::string& out, size_t n, int fix_base, size_t fix_limit, int c8, int c16,
                int c32) {
  if (fix_base >= 0 && n < fix_limit) {
    out.push_back(char(fix_base | int(n)));
  } else if (c8 >= 0 && n <= 0xff) {
    out.push_back(char(c8));
    put_be(out, n, 1);
  } else if (n <= 0xffff) {
    out.push_back(char(c16));
    put_be(out, n, 2);
  } else {
    out.push_back(char(c32));
    put_be(out, n, 4);
  }
}

}  // namespace

void encode(const Value& value, std::string& out) {
  std::visit(
      [&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, Nil>) {
          out.push_back(char(0xc0));
        } else if constexpr (std::is_same_v<T, bool>) {
          out.push_back(char(x ? 0xc3 : 0xc2));
        } else if constexpr (std::is_same_v<T, int64_t>) {
          put_int(out, x);
        } else if constexpr (std::is_same_v<T, uint64_t>) {
          put_uint(out, x);
        } else if constexpr (std::is_same_v<T, double>) {
          out.push_back(char(0xcb));
          put_be(out, std::bit_cast<uint64_t>(x), 8);
        } else if constexpr (std::is_same_v<T, std::string>) {
          put_header(out, x.size(), 0xa0, 32, 0xd9, 0xda, 0xdb);
          out += x;
        } else if constexpr (std::is_same_v<T, Bin>) {
          put_header(out, x.data.size(), -1, 0, 0xc4, 0xc5, 0xc6);
          out += x.data;
        } else if constexpr (std::is_same_v<T, Array>) {
          put_header(out, x.size(), 0x90, 16, -1, 0xdc, 0xdd);
          for (const auto& e : x) encode(e, out);
        } else if constexpr (std::is_same_v<T, Map>) {
          put_header(out, x.size(), 0x80, 16, -1, 0xde, 0xdf);
          for (const auto& [k, val] : x) {
            encode(k, out);
            encode(val, out);
          }
        } else if constexpr (std::is_same_v<T, Ext>) {
          const size_t n = x.data.size();
          if (n == 1 || n == 2 || n == 4 || n == 8 || n == 16) {
            out.push_back(char(0xd4 + std::countr_zero(n)));
          } else if (n <= 0xff) {
            out.push_back(char(0xc7));
            put_be(out, n, 1);
          } else if (n <= 0xffff) {
            out.push_back(char(0xc8));
            put_be(out, n, 2);
          } else {
            out.push_back(char(0xc9));
            put_be(out, n, 4);
          }
          out.push_back(char(x.type));
          out += x.data;
        }
      },
      value.v);
}

std::string encode(const Value& value) {
  std::string out;
  encode(value, out);
  return out;
}

// ---- Decoder ------------------------------------------------------------------------

namespace {

constexpr size_t kMaxBytes = 64u << 20;  // a single str/bin; the host is trusted, not infinite
constexpr size_t kMaxItems = 1u << 24;
constexpr int kMaxDepth = 64;

struct Incomplete {};

struct Reader {
  std::string_view d;
  size_t p;
  int depth = 0;

  void need(size_t n) const {
    if (d.size() - p < n) throw Incomplete{};
  }
  uint8_t u8() {
    need(1);
    return uint8_t(d[p++]);
  }
  uint64_t be(int n) {
    need(size_t(n));
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | uint8_t(d[p++]);
    return v;
  }
  std::string bytes(uint64_t n) {
    if (n > kMaxBytes) throw DecodeError("msgpack: string too long");
    need(size_t(n));
    std::string s(d.substr(p, size_t(n)));
    p += size_t(n);
    return s;
  }
  Value array(uint64_t n) {
    if (n > kMaxItems) throw DecodeError("msgpack: array too long");
    Array a;
    a.reserve(std::min<uint64_t>(n, 1024));
    for (uint64_t i = 0; i < n; ++i) a.push_back(value());
    return a;
  }
  Value map(uint64_t n) {
    if (n > kMaxItems) throw DecodeError("msgpack: map too long");
    Map m;
    m.reserve(std::min<uint64_t>(n, 1024));
    for (uint64_t i = 0; i < n; ++i) {
      Value k = value();
      Value v = value();
      m.emplace_back(std::move(k), std::move(v));
    }
    return m;
  }
  Value ext(uint64_t n) {
    auto type = int8_t(u8());
    return Ext{type, bytes(n)};
  }

  Value value() {
    if (++depth > kMaxDepth) throw DecodeError("msgpack: nesting too deep");
    struct Guard {
      int& d;
      ~Guard() { --d; }
    } guard{depth};

    const uint8_t c = u8();
    if (c <= 0x7f) return int64_t{c};
    if (c >= 0xe0) return int64_t{int8_t(c)};
    if ((c & 0xf0) == 0x80) return map(c & 0x0f);
    if ((c & 0xf0) == 0x90) return array(c & 0x0f);
    if ((c & 0xe0) == 0xa0) return bytes(c & 0x1f);
    switch (c) {
      case 0xc0: return nullptr;
      case 0xc2: return false;
      case 0xc3: return true;
      case 0xc4: return Bin{bytes(be(1))};
      case 0xc5: return Bin{bytes(be(2))};
      case 0xc6: return Bin{bytes(be(4))};
      case 0xc7: return ext(be(1));
      case 0xc8: return ext(be(2));
      case 0xc9: return ext(be(4));
      case 0xca: return double(std::bit_cast<float>(uint32_t(be(4))));
      case 0xcb: return std::bit_cast<double>(be(8));
      case 0xcc: return int64_t(be(1));
      case 0xcd: return int64_t(be(2));
      case 0xce: return int64_t(be(4));
      case 0xcf: {
        const uint64_t u = be(8);
        if (u <= uint64_t(std::numeric_limits<int64_t>::max())) return int64_t(u);
        return u;
      }
      case 0xd0: return int64_t{int8_t(be(1))};
      case 0xd1: return int64_t{int16_t(be(2))};
      case 0xd2: return int64_t{int32_t(be(4))};
      case 0xd3: return int64_t(be(8));
      case 0xd4: return ext(1);
      case 0xd5: return ext(2);
      case 0xd6: return ext(4);
      case 0xd7: return ext(8);
      case 0xd8: return ext(16);
      case 0xd9: return bytes(be(1));
      case 0xda: return bytes(be(2));
      case 0xdb: return bytes(be(4));
      case 0xdc: return array(be(2));
      case 0xdd: return array(be(4));
      case 0xde: return map(be(2));
      case 0xdf: return map(be(4));
      default: throw DecodeError("msgpack: invalid type byte");
    }
  }
};

}  // namespace

std::optional<Value> decode(std::string_view data, size_t& pos) {
  Reader r{data, pos};
  try {
    Value v = r.value();
    pos = r.p;
    return v;
  } catch (const Incomplete&) {
    return std::nullopt;
  }
}

void Decoder::feed(std::string_view bytes) {
  if (pos_ > 0 && pos_ == buf_.size()) {
    buf_.clear();
    pos_ = 0;
  }
  buf_.append(bytes);
}

std::optional<Value> Decoder::next() {
  auto v = decode(buf_, pos_);
  if (pos_ == buf_.size()) {
    buf_.clear();
    pos_ = 0;
  } else if (!v && pos_ > 0) {
    buf_.erase(0, pos_);
    pos_ = 0;
  }
  return v;
}

}  // namespace rcp::mp
