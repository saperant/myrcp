#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace rcp::utf8 {

// Length of the sequence a lead byte starts (1..4), 0 for a continuation/invalid byte.
int seq_len(unsigned char lead);
// Decodes the code point at s[i] and advances i. Invalid input yields U+FFFD (1 byte).
char32_t decode(std::string_view s, size_t& i);
void append(std::string& out, char32_t c);
// Terminal columns of a code point: 0 (combining, control), 1 or 2 (wide).
// Locale-independent, so layout is the same everywhere.
int width(char32_t c);
int width(std::string_view s);
// Byte offset of the code point before / after offset i.
size_t prev(std::string_view s, size_t i);
size_t next(std::string_view s, size_t i);

}  // namespace rcp::utf8
