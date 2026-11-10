// The `myrcp` (and `myrcp-gtk`) launcher: a workbench for the plugins on
// $MYRCP_PLUGIN_PATH, or for an application's product directory, so an application
// written in Python needs no program of its own:
//
//   myrcp --product ~/src/songs/myrcp     # or MYRCP_PRODUCT=~/src/songs/myrcp myrcp
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "rcp/app.hpp"

int main(int argc, char** argv) {
  rcp::AppInfo app;
#ifdef MYRCP_LAUNCH_GTK
  app.name = "myrcp-gtk";
#endif
  std::string product;
  if (const char* e = std::getenv("MYRCP_PRODUCT"); e && *e) product = e;
  std::vector<char*> rest{argv[0]};
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--product" && i + 1 < argc) product = argv[++i];
    else if (a.starts_with("--product=")) product = a.substr(10);
    else rest.push_back(argv[i]);
    if (a == "--help" || a == "-h") {
      std::puts("  --product DIR    an application's product directory (product.toml and its plugins;\n"
                "                   also $MYRCP_PRODUCT)\n");
    }
  }
  if (!product.empty()) {
    std::string error;
    if (!rcp::load_product(product, app, error)) {
      std::fprintf(stderr, "%s: %s\n", argv[0], error.c_str());
      return 2;
    }
  }
  rest.push_back(nullptr);
#ifdef MYRCP_LAUNCH_GTK
  return rcp::run_gtk(int(rest.size()) - 1, rest.data(), app);
#else
  return rcp::run(int(rest.size()) - 1, rest.data(), app);
#endif
}
