// Navigation history: the places the user has been (a view, its focused widget and the
// row there), for Back and Forward as in Eclipse and NetBeans (Alt+← / Alt+→).
// Front-ends record the place they leave whenever another view takes the focus.
#pragma once

#include <deque>
#include <functional>
#include <optional>
#include <string>

namespace rcp {

struct Place {
  std::string view, widget, key;  // key: the row's, when the widget has rows
  int cursor = 0;                 // its index, for when the key is not found
  bool operator==(const Place&) const = default;
};

class NavHistory {
 public:
  static constexpr size_t kMax = 100;

  // Leaving `from` for somewhere else: it is where Back goes; Forward is forgotten.
  void left(const Place& from) {
    if (from.view.empty()) return;
    if (!back_.empty() && back_.back().view == from.view) back_.back() = from;  // the same view: its latest row
    else back_.push_back(from);
    if (back_.size() > kMax) back_.pop_front();
    forward_.clear();
  }

  // Back from `here`: the latest place that still `exists`; here goes to Forward.
  std::optional<Place> back(const Place& here, const std::function<bool(const Place&)>& exists) {
    return step(back_, forward_, here, exists);
  }
  std::optional<Place> forward(const Place& here, const std::function<bool(const Place&)>& exists) {
    return step(forward_, back_, here, exists);
  }
  bool can_back() const { return !back_.empty(); }
  bool can_forward() const { return !forward_.empty(); }

 private:
  static std::optional<Place> step(std::deque<Place>& from, std::deque<Place>& to, const Place& here,
                                   const std::function<bool(const Place&)>& exists) {
    while (!from.empty()) {
      Place p = std::move(from.back());
      from.pop_back();
      if (p.view == here.view || !exists(p)) continue;  // gone, or where we are
      if (!here.view.empty()) to.push_back(here);
      return p;
    }
    return std::nullopt;
  }

  std::deque<Place> back_, forward_;
};

}  // namespace rcp
