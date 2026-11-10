#!/bin/sh
# myrcp's build helper. ./build.sh help lists the commands.
set -eu

cd "$(dirname "$0")"
jobs=$(nproc 2>/dev/null || echo 4)
generator=""
command -v ninja >/dev/null 2>&1 && generator="-G Ninja"

# configure DIR BUILD_TYPE [-DNAME=VALUE...]: once, or again when the type or an option changes.
configure() {
  dir=$1 type=$2
  shift 2
  again=no
  for a in "$@"; do
    kv=${a#-D}
    grep -q "^${kv%%=*}:[A-Z]*=${kv#*=}\$" "$dir/CMakeCache.txt" 2>/dev/null || again=yes
  done
  if [ $again = yes ] || [ ! -f "$dir/CMakeCache.txt" ] || ! grep -q "^CMAKE_BUILD_TYPE:STRING=$type\$" "$dir/CMakeCache.txt"; then
    # shellcheck disable=SC2086
    cmake -S . -B "$dir" $generator -DCMAKE_BUILD_TYPE="$type" "$@"
  fi
}

compile() { cmake --build "$1" -j "$jobs"; }

unit_tests() { "$1/rcp_tests"; }

# build/: libmyrcp and the launcher; with `demo` (an argument) the demo application too.
dev() {
  configure build RelWithDebInfo -DMYRCP_DEMO=${1:-OFF}
  compile build
  unit_tests build
}

# The Python tests (SDK, plugin host, the demo application in a pty), in .venv (made with uv).
python_tests() {
  if [ ! -x .venv/bin/pytest ]; then
    command -v uv >/dev/null 2>&1 || { echo "build.sh: the Python tests need uv (or .venv with pytest and pyte)" >&2; exit 1; }
    uv venv
    uv pip install pytest pyte
  fi
  (cd python && ../.venv/bin/pytest -q "$@")
}

# The GTK workbench (libmyrcp-gtk, myrcp-gtk and the demo's) in build-gtk/; its smoke
# test runs with ./build.sh test.
gtk() {
  configure build-gtk RelWithDebInfo -DMYRCP_GTK=ON -DMYRCP_DEMO=ON
  compile build-gtk
}

# MYRCP_GTK=ON ./build.sh release (or install): libmyrcp-gtk and myrcp-gtk too.
gtk_flag=-DMYRCP_GTK=${MYRCP_GTK:-OFF}

# The release build, tested, and a stripped copy (libraries, launcher, header, SDK) in build-release/dist.
release() {
  configure build-release Release "$gtk_flag" -DMYRCP_DEMO=OFF
  compile build-release
  unit_tests build-release
  rm -rf build-release/dist
  cmake --install build-release --strip --prefix build-release/dist >/dev/null
}

usage() {
  cat <<'EOF'
Usage: ./build.sh [command]

  dev            (the default) libmyrcp and the launcher, optimised with debug info, in build/,
                 and the unit tests
  demo [ARGS]    dev with the demo application (examples/demo), and start it with ARGS
  test [ARGS]    dev with the demo, then the Python tests (pytest ARGS; .venv made with uv if missing)
  release        optimised, no debug info, the unit tests; stripped into build-release/dist
  install [DIR]  the release build, stripped, into DIR (default ~/.local: bin/myrcp, lib/libmyrcp.so,
                 include/rcp/app.hpp, the CMake package and myrcp.pc, the SDK in share/myrcp)
  asan           with AddressSanitizer and UBSan, in build-asan/, and the unit tests
  gtk [ARGS]     the GTK 4 workbench in build-gtk/ (needs gtk4, optionally vte-2.91-gtk4) and
                 start the demo in it with ARGS; MYRCP_GTK=ON with release/install builds it too
  size           the release library's size, before and after stripping, and what it links
  clean          remove build/, build-release/, build-asan/ and build-gtk/
  help           this text
EOF
}

cmd=${1:-dev}
[ $# -gt 0 ] && shift

case $cmd in
  dev)
    dev
    ;;
  test)
    dev ON
    python_tests "$@"
    ;;
  demo)
    dev ON
    exec build/examples/demo/myrcp-demo "$@"
    ;;
  release)
    release
    ls -l build-release/dist/bin/
    ;;
  install)
    prefix=${1:-$HOME/.local}
    release
    rm -rf "$prefix/share/myrcp"
    cmake --install build-release --strip --prefix "$prefix"
    case ":$PATH:" in
      *":$prefix/bin:"*) ;;
      *) echo "Note: $prefix/bin is not on your PATH" ;;
    esac
    ;;
  asan)
    configure build-asan Debug -DMYRCP_SANITIZE=ON
    compile build-asan
    unit_tests build-asan
    ;;
  gtk)
    gtk
    exec build-gtk/examples/demo/myrcp-demo-gtk "$@"
    ;;
  size)
    release >/dev/null
    lib=build-release/libmyrcp.so.0
    echo "$lib: $(wc -c <"$lib") bytes, stripped $(wc -c <build-release/dist/lib*/libmyrcp.so.0) bytes"
    ldd "$lib" | awk '$1 !~ /vdso/ {print "  links " $1}'
    ;;
  clean)
    rm -rf build build-release build-asan build-gtk
    ;;
  help | -h | --help)
    usage
    ;;
  *)
    echo "build.sh: no command '$cmd'" >&2
    usage >&2
    exit 2
    ;;
esac
