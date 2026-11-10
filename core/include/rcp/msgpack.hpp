// msgpack values, encoder and streaming decoder. The plugin protocol and Neovim's
// --embed API both speak msgpack-RPC.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace rcp::mp {

struct Value;
using Array = std::vector<Value>;
using Map = std::vector<std::pair<Value, Value>>;  // keeps order; keys of any type

struct Nil {
  bool operator==(const Nil&) const = default;
};
struct Bin {
  std::string data;
  bool operator==(const Bin&) const = default;
};
struct Ext {  // Neovim sends Buffer/Window/Tabpage handles as ext types
  int8_t type = 0;
  std::string data;
  bool operator==(const Ext&) const = default;
};

struct DecodeError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Value {
  std::variant<Nil, bool, int64_t, uint64_t, double, std::string, Bin, Array, Map, Ext> v;

  Value() = default;
  Value(std::nullptr_t) {}
  Value(bool b) : v(b) {}
  Value(int i) : v(int64_t{i}) {}
  Value(long i) : v(int64_t{i}) {}
  Value(long long i) : v(int64_t{i}) {}
  Value(unsigned i) : v(int64_t{i}) {}
  Value(unsigned long u) : v(uint64_t{u}) {}
  Value(unsigned long long u) : v(uint64_t{u}) {}
  Value(double d) : v(d) {}
  Value(const char* s) : v(std::string(s)) {}
  Value(std::string s) : v(std::move(s)) {}
  Value(std::string_view s) : v(std::string(s)) {}
  Value(Bin b) : v(std::move(b)) {}
  Value(Array a) : v(std::move(a)) {}
  Value(Map m) : v(std::move(m)) {}
  Value(Ext e) : v(std::move(e)) {}

  bool is_nil() const { return std::holds_alternative<Nil>(v); }
  bool is_str() const { return std::holds_alternative<std::string>(v); }
  bool is_array() const { return std::holds_alternative<Array>(v); }
  bool is_map() const { return std::holds_alternative<Map>(v); }
  bool is_int() const {
    return std::holds_alternative<int64_t>(v) || std::holds_alternative<uint64_t>(v);
  }
  bool is_number() const { return is_int() || std::holds_alternative<double>(v); }

  // Lenient accessors: the wrong type yields the default.
  std::string_view str(std::string_view def = {}) const;
  int64_t integer(int64_t def = 0) const;
  double number(double def = 0) const;
  bool boolean(bool def = false) const;
  const Array& array() const;
  const Map& map() const;
  // Map lookup by string key; nil when absent.
  const Value& operator[](std::string_view key) const;
  // Array element; nil when out of range.
  const Value& at(size_t i) const;

  static const Value& nil();
};

bool operator==(const Value& a, const Value& b);

void encode(const Value& value, std::string& out);
std::string encode(const Value& value);

// Decodes one value starting at `pos`. Returns nullopt (pos unchanged) when the data
// ends mid-value; throws DecodeError on malformed input.
std::optional<Value> decode(std::string_view data, size_t& pos);

// Feed bytes as they arrive, pop complete values.
class Decoder {
 public:
  void feed(std::string_view bytes);
  std::optional<Value> next();
  size_t buffered() const { return buf_.size() - pos_; }

 private:
  std::string buf_;
  size_t pos_ = 0;
};

}  // namespace rcp::mp
