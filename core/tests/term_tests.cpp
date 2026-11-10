// Terminal views: a real program in a pty, its screen through libvterm.
#include <chrono>
#include <cstdio>
#include <poll.h>
#include <string>

#include "term.hpp"

namespace {

int failures = 0, checks = 0;
#define TCHECK(cond)                                                       \
  do {                                                                     \
    ++checks;                                                              \
    if (!(cond)) {                                                         \
      ++failures;                                                          \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                      \
  } while (0)

using namespace rcp;

// Pumps the program's output until pred() or a timeout.
template <class P>
bool pump(TermSession& t, P pred, int ms = 3000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    pollfd p{t.fd(), POLLIN, 0};
    if (poll(&p, 1, 20) > 0 && !t.on_readable()) return pred();
  }
  return pred();
}

std::string row(TermSession& t, int y) {
  Grid g(t.cols(), t.rows());
  t.draw(g, {0, 0, t.cols(), t.rows()}, false);
  return g.row_text(y);
}

}  // namespace

int term_tests() {
  std::string err;
  // Colours, keys back to the program, its exit.
  auto t = TermSession::start(
      {{"/bin/sh", "-c", "printf 'hello \\033[31mred\\033[0m\\n'; read line; printf 'got %s\\n' \"$line\"; exit 3"}, "", 40, 5},
      err);
  TCHECK(t != nullptr);
  if (!t) return 1;
  TCHECK(pump(*t, [&] { return row(*t, 0).starts_with("hello red"); }));
  Grid g(40, 5);
  t->draw(g, {0, 0, 40, 5}, false);
  TCHECK(g.at(6, 0).style == Style::raw);
  TCHECK(g.at(6, 0).fg == (kColorIndexed | 1));  // red
  TCHECK(g.at(0, 0).fg == 0);                    // the theme's default
  for (const char* k : {"o", "k"}) t->key(Key{k, k});
  t->key(Key{"enter"});
  TCHECK(pump(*t, [&] { return t->exited(); }));
  TCHECK(t->exit_status() == 3);

  // A program that asks for the mouse (as vim's mouse=a does) gets it; one that stops, not.
  auto m = TermSession::start({{"/bin/sh", "-c", "printf '\\033[?1006;1000h\\033[?1002hon\\n'; read a; "
                                 "printf '\\033[?1002l\\033[?1000l\\033[?1006loff\\n'; read b"}, "", 40, 5}, err);
  TCHECK(m != nullptr);
  if (m) {
    TCHECK(pump(*m, [&] { return row(*m, 0).starts_with("on"); }));
    TCHECK(m->wants_mouse());
    m->key(Key{"enter"});
    TCHECK(pump(*m, [&] { return row(*m, 1).starts_with("off") || row(*m, 2).starts_with("off"); }));
    TCHECK(!m->wants_mouse());
  }
  TCHECK(row(*t, 2).starts_with("got ok"));

  // Resizing tells the program (stty sees the new size).
  auto s = TermSession::start({{"/bin/sh", "-c", "read x; stty size"}, "", 30, 4}, err);
  TCHECK(s != nullptr);
  if (!s) return 1;
  s->resize(50, 7);
  s->key(Key{"enter"});
  TCHECK(pump(*s, [&] { return s->exited(); }));
  bool saw = false;
  for (int y = 0; y < 7; ++y) saw |= row(*s, y).starts_with("7 50");
  TCHECK(saw);

  std::printf("term: %d checks, %d failures\n", checks, failures);
  return failures;
}
