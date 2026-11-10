#include "rcp/utf8.hpp"

namespace rcp::utf8 {

int seq_len(unsigned char lead) {
  if (lead < 0x80) return 1;
  if ((lead & 0xe0) == 0xc0) return 2;
  if ((lead & 0xf0) == 0xe0) return 3;
  if ((lead & 0xf8) == 0xf0) return 4;
  return 0;
}

char32_t decode(std::string_view s, size_t& i) {
  const auto lead = static_cast<unsigned char>(s[i]);
  const int n = seq_len(lead);
  if (n == 1) {
    ++i;
    return lead;
  }
  if (n == 0 || i + size_t(n) > s.size()) {
    ++i;
    return 0xfffd;
  }
  char32_t c = lead & (0x7f >> n);
  for (int k = 1; k < n; ++k) {
    const auto b = static_cast<unsigned char>(s[i + size_t(k)]);
    if ((b & 0xc0) != 0x80) {
      ++i;
      return 0xfffd;
    }
    c = (c << 6) | (b & 0x3f);
  }
  i += size_t(n);
  return c;
}

void append(std::string& out, char32_t c) {
  if (c < 0x80) {
    out.push_back(char(c));
  } else if (c < 0x800) {
    out.push_back(char(0xc0 | (c >> 6)));
    out.push_back(char(0x80 | (c & 0x3f)));
  } else if (c < 0x10000) {
    out.push_back(char(0xe0 | (c >> 12)));
    out.push_back(char(0x80 | ((c >> 6) & 0x3f)));
    out.push_back(char(0x80 | (c & 0x3f)));
  } else {
    out.push_back(char(0xf0 | (c >> 18)));
    out.push_back(char(0x80 | ((c >> 12) & 0x3f)));
    out.push_back(char(0x80 | ((c >> 6) & 0x3f)));
    out.push_back(char(0x80 | (c & 0x3f)));
  }
}

int width(char32_t c) {
  if (c < 0x20 || (c >= 0x7f && c < 0xa0)) return 0;
  if (c < 0x300) return 1;
  // Combining marks and zero-width characters.
  if ((c >= 0x300 && c <= 0x36f) || (c >= 0x200b && c <= 0x200f) || (c >= 0xfe00 && c <= 0xfe0f) ||
      (c >= 0x1ab0 && c <= 0x1aff) || (c >= 0x20d0 && c <= 0x20ff))
    return 0;
  // East Asian wide and emoji blocks.
  if ((c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0x303e) || (c >= 0x3041 && c <= 0xa4cf) ||
      (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe30 && c <= 0xfe4f) ||
      (c >= 0xff00 && c <= 0xff60) || (c >= 0xffe0 && c <= 0xffe6) || (c >= 0x1f300 && c <= 0x1f64f) ||
      (c >= 0x1f900 && c <= 0x1f9ff) || (c >= 0x20000 && c <= 0x3fffd))
    return 2;
  return 1;
}

int width(std::string_view s) {
  int w = 0;
  for (size_t i = 0; i < s.size();) w += width(decode(s, i));
  return w;
}

size_t prev(std::string_view s, size_t i) {
  if (i == 0) return 0;
  --i;
  while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xc0) == 0x80) --i;
  return i;
}

size_t next(std::string_view s, size_t i) {
  if (i >= s.size()) return s.size();
  decode(s, i);
  return i;
}

}  // namespace rcp::utf8
