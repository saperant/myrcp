// JSON to and from msgpack values: what Vim's channels speak ("json" mode, one message
// per line). Maps keep their order; numbers without a fraction or exponent are integers.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "rcp/msgpack.hpp"

namespace rcp {

std::string to_json(const mp::Value& v);
// One JSON value (surrounding white space allowed); nullopt when it is not one.
std::optional<mp::Value> from_json(std::string_view text);

}  // namespace rcp
