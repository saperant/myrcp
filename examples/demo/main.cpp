// myrcp-demo: an application on libmyrcp, the smallest there is: a name and the
// directory of its plugins. Built with -DMYRCP_DEMO=ON (./build.sh demo), for
// developing myrcp itself; it is not installed.
#include "rcp/app.hpp"

int main(int argc, char** argv) {
  rcp::AppInfo app;
  app.id = "myrcp-demo";  // its own perspectives and log: $XDG_STATE_HOME/myrcp-demo/
  app.name = "myrcp";  // the title bar, --help and --version
  app.plugin_dirs = {MYRCP_DEMO_PLUGINS};
#ifdef MYRCP_DEMO_GTK
  return rcp::run_gtk(argc, argv, app);
#else
  return rcp::run(argc, argv, app);
#endif
}
