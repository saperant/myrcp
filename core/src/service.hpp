// Services: the workbench's own features with views, built into the hub rather than
// contributed by a plugin (Search, Tasks, Outline). A service declares views, commands
// and preferences like a manifest does, draws its views' trees itself, may run
// programs whose output the hub's event loop reads, and hears the selection.
#pragma once

#include <memory>
#include <poll.h>
#include <string>
#include <string_view>
#include <vector>

#include "prefs.hpp"
#include "rcp/msgpack.hpp"

namespace rcp {

struct CommandInfo;
struct ViewInfo;
class Hub;

class Service {
 public:
  explicit Service(Hub& hub) : hub_(hub) {}
  virtual ~Service() = default;
  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;

  // What it contributes (commands' `plugin` is filled in by the hub).
  virtual std::vector<ViewInfo> views() const = 0;
  virtual std::vector<CommandInfo> commands() const = 0;
  virtual std::vector<PrefInfo> prefs() const { return {}; }
  bool owns_view(std::string_view id) const;
  bool owns_command(std::string_view id) const;

  // Its views: opened (tree() comes next), closed, events from them.
  virtual void opened(const std::string& view) { (void)view; }
  virtual void closed(const std::string& view) { (void)view; }
  virtual mp::Value tree(const std::string& view) = 0;
  virtual std::string title(const std::string& view) = 0;
  virtual void event(const std::string& view, std::string_view widget, std::string_view kind, const mp::Value& value) {
    (void)view, (void)widget, (void)kind, (void)value;
  }
  // `view`: the focused view's id. Returns false when the command does not apply there.
  virtual bool command(const std::string& id, const std::string& view) = 0;

  // The selection changed (`from`: the view it was made in; items as on the wire).
  virtual void selection_changed(const std::string& from, const mp::Value& items) { (void)from, (void)items; }
  virtual void prefs_changed() {}

  // Programs it runs: their fds to poll, and what to do when one is readable.
  virtual void add_fds(std::vector<pollfd>& fds) const { (void)fds; }
  virtual void on_readable(int fd) { (void)fd; }

 protected:
  Hub& hub_;
};

// The ones every workbench has (search.cpp, outline.cpp).
std::unique_ptr<Service> make_search_service(Hub& hub);
std::unique_ptr<Service> make_outline_service(Hub& hub);

}  // namespace rcp
