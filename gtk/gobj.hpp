// Small GObject helpers for C++: signals into lambdas, and owned references.
#pragma once

#include <functional>
#include <type_traits>
#include <utility>

#include <gtk/gtk.h>

namespace rcp::gtk {

// Connects `fn` to a signal. Args are the signal's C arguments, the instance first,
// without the trailing user data: on<void, GtkButton*>(button, "clicked", ...).
// The function is freed when the handler is disconnected or the instance finalized.
template <typename Ret, typename... Args>
gulong on(gpointer instance, const char* signal, std::type_identity_t<std::function<Ret(Args...)>> fn, bool after = false) {
  using F = std::function<Ret(Args...)>;
  auto* data = new F(std::move(fn));
  GCallback cb = G_CALLBACK(+[](Args... args, gpointer d) -> Ret { return (*static_cast<F*>(d))(args...); });
  return g_signal_connect_data(instance, signal, cb, data, [](gpointer d, GClosure*) { delete static_cast<F*>(d); },
                               after ? G_CONNECT_AFTER : GConnectFlags(0));
}

// A strong reference to a GObject (floating ones are sunk).
template <typename T>
class Ref {
 public:
  Ref() = default;
  explicit Ref(T* p) : p_(p) {
    if (p_) g_object_ref_sink(p_);
  }
  ~Ref() { reset(); }
  Ref(const Ref& o) : p_(o.p_) {
    if (p_) g_object_ref(p_);
  }
  Ref(Ref&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
  Ref& operator=(Ref o) noexcept {
    std::swap(p_, o.p_);
    return *this;
  }
  void reset() {
    if (p_) g_object_unref(std::exchange(p_, nullptr));
  }
  T* get() const { return p_; }
  operator T*() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }

 private:
  T* p_ = nullptr;
};

// Takes an already owned (full) reference, as get_item() and *_new() for non-widgets give.
template <typename T>
Ref<T> adopt(T* p) {
  Ref<T> r(p);  // ref_sink on a non-floating object adds a ref ...
  if (p) g_object_unref(p);  // ... so drop the one we were given
  return r;
}

}  // namespace rcp::gtk
