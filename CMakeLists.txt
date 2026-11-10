cmake_minimum_required(VERSION 3.24)
project(myrcp VERSION 0.9.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(MYRCP_GTK "Build libmyrcp-gtk and myrcp-gtk, the workbench as a GTK 4 application" OFF)
option(MYRCP_DEMO "Build the demo application (examples/demo), for developing myrcp itself" OFF)
option(MYRCP_TESTS "Build the unit tests" ON)
option(MYRCP_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
if(MYRCP_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

include(GNUInstallDirs)
# The libraries export rcp/app.hpp's API and nothing else.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(warnings -Wall -Wextra -Wpedantic)

# ---- internal static libraries (linked into libmyrcp, and into the unit tests) -------

# No I/O: msgpack, grid, keys, layout.
add_library(rcp_core STATIC
  core/src/msgpack.cpp core/src/json.cpp core/src/utf8.cpp core/src/grid.cpp core/src/keys.cpp core/src/layout.cpp)
target_include_directories(rcp_core PUBLIC core/include)
target_compile_options(rcp_core PRIVATE ${warnings})

# Plugin hosts, the workbench model and its services (Search, Tasks, Outline), shared
# by the front-ends. A build tree's SDK is the source tree's; an installed one is found
# next to the library (host.cpp).
add_library(rcp_hub STATIC
  core/src/console.cpp core/src/process.cpp core/src/rpc.cpp core/src/host.cpp core/src/hub.cpp
  core/src/hub_prefs.cpp core/src/prefs.cpp core/src/search.cpp core/src/outline.cpp)
target_include_directories(rcp_hub PUBLIC core/src)
target_link_libraries(rcp_hub PUBLIC rcp_core ${CMAKE_DL_LIBS})
target_compile_options(rcp_hub PRIVATE ${warnings})
target_compile_definitions(rcp_hub PRIVATE MYRCP_PYTHON_DIR="${CMAKE_SOURCE_DIR}/python")

# Editors linked both ways (Neovim, Vim).
add_library(rcp_editors STATIC core/src/editor_links.cpp core/src/nvim.cpp core/src/vim.cpp)
target_link_libraries(rcp_editors PUBLIC rcp_hub)
target_compile_options(rcp_editors PRIVATE ${warnings})

# Terminal views: programs in a pty, emulated by libvterm (optional).
find_package(PkgConfig)
if(PkgConfig_FOUND)
  pkg_check_modules(VTERM IMPORTED_TARGET vterm)
endif()
if(VTERM_FOUND)
  add_library(rcp_term STATIC core/src/term.cpp)
  target_link_libraries(rcp_term PUBLIC rcp_core PkgConfig::VTERM util)
  target_compile_options(rcp_term PRIVATE ${warnings})
else()
  message(STATUS "myrcp: no libvterm (pkg-config vterm); building without terminal views")
endif()

# ---- libmyrcp: the terminal workbench ------------------------------------------------

add_library(myrcp SHARED core/src/app.cpp core/src/terminal.cpp core/src/workbench.cpp)
add_library(myrcp::myrcp ALIAS myrcp)
# A wheel (scikit-build-core sets SKBUILD) holds no symlinks: one file, libmyrcp.so.
if(NOT SKBUILD)
  set_target_properties(myrcp PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
endif()
target_include_directories(myrcp PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/core/include>
                                        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(myrcp PRIVATE $<BUILD_INTERFACE:rcp_editors>)
target_compile_options(myrcp PRIVATE ${warnings})
target_compile_definitions(myrcp PRIVATE MYRCP_VERSION="${PROJECT_VERSION}")
if(VTERM_FOUND)
  target_link_libraries(myrcp PRIVATE $<BUILD_INTERFACE:rcp_term>)
  target_compile_definitions(myrcp PRIVATE MYRCP_HAVE_VTERM=1)
endif()

# The launcher: `myrcp` for $MYRCP_PLUGIN_PATH, `myrcp --product DIR` for an application.
add_executable(myrcp-launcher core/src/launcher.cpp)
set_target_properties(myrcp-launcher PROPERTIES OUTPUT_NAME myrcp
                      INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
target_link_libraries(myrcp-launcher PRIVATE myrcp)
target_compile_options(myrcp-launcher PRIVATE ${warnings})

set(install_targets myrcp myrcp-launcher)

# ---- libmyrcp-gtk: the GTK 4 workbench (-DMYRCP_GTK=ON) ------------------------------

# The same hub and views as native GTK widgets; terminal views (shells, editors) with
# VTE when it is there.
if(MYRCP_GTK)
  pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.12)
  pkg_check_modules(VTE_GTK4 IMPORTED_TARGET vte-2.91-gtk4)
  add_library(myrcp-gtk SHARED gtk/app_gtk.cpp gtk/workbench.cpp gtk/render.cpp)
  add_library(myrcp::myrcp-gtk ALIAS myrcp-gtk)
  set_target_properties(myrcp-gtk PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
  target_include_directories(myrcp-gtk PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/core/include>
                                              $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  # On libmyrcp too: version(), load_product().
  target_link_libraries(myrcp-gtk PUBLIC myrcp PRIVATE $<BUILD_INTERFACE:rcp_editors> PkgConfig::GTK4)
  target_compile_options(myrcp-gtk PRIVATE -Wall -Wextra)
  if(VTE_GTK4_FOUND)
    target_link_libraries(myrcp-gtk PRIVATE PkgConfig::VTE_GTK4)
    target_compile_definitions(myrcp-gtk PRIVATE MYRCP_HAVE_VTE=1)
  else()
    message(STATUS "myrcp-gtk: no VTE (pkg-config vte-2.91-gtk4); building without terminal views")
  endif()
  add_executable(myrcp-gtk-launcher core/src/launcher.cpp)
  set_target_properties(myrcp-gtk-launcher PROPERTIES OUTPUT_NAME myrcp-gtk
                        INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
  target_compile_definitions(myrcp-gtk-launcher PRIVATE MYRCP_LAUNCH_GTK=1)
  target_link_libraries(myrcp-gtk-launcher PRIVATE myrcp-gtk)
  list(APPEND install_targets myrcp-gtk myrcp-gtk-launcher)
endif()

# ---- the demo application: a consumer of libmyrcp, not installed --------------------

if(MYRCP_DEMO)
  add_subdirectory(examples/demo)
endif()

# ---- install: libraries, launcher, header, SDK, CMake package, pkg-config ------------

install(TARGETS ${install_targets} EXPORT myrcpTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES core/include/rcp/app.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rcp)
install(DIRECTORY python/myrcp DESTINATION ${CMAKE_INSTALL_DATADIR}/myrcp/python PATTERN __pycache__ EXCLUDE)
install(EXPORT myrcpTargets NAMESPACE myrcp:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/myrcp)
include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/myrcpConfig.cmake.in ${CMAKE_BINARY_DIR}/myrcpConfig.cmake
                              INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/myrcp)
write_basic_package_version_file(${CMAKE_BINARY_DIR}/myrcpConfigVersion.cmake COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_BINARY_DIR}/myrcpConfig.cmake ${CMAKE_BINARY_DIR}/myrcpConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/myrcp)
configure_file(cmake/myrcp.pc.in ${CMAKE_BINARY_DIR}/myrcp.pc @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/myrcp.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# ---- unit tests ----------------------------------------------------------------------

if(MYRCP_TESTS)
  enable_testing()
  add_executable(rcp_tests core/tests/tests.cpp core/tests/service_tests.cpp)
  target_link_libraries(rcp_tests PRIVATE rcp_core rcp_hub)
  if(VTERM_FOUND)
    target_sources(rcp_tests PRIVATE core/tests/term_tests.cpp)
    target_link_libraries(rcp_tests PRIVATE rcp_term)
    target_compile_definitions(rcp_tests PRIVATE MYRCP_HAVE_VTERM=1)
  endif()
  add_test(NAME rcp_tests COMMAND rcp_tests)
endif()
