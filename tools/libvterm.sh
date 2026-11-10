#!/bin/sh
# libvterm from source into /usr/local, for building wheels in a manylinux container
# (cibuildwheel's before-all): the distributions there have none, or an old one.
set -eu
version=0.3.3
sha256=09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0
if command -v dnf >/dev/null 2>&1; then dnf install -y -q libtool perl curl; fi
cd /tmp
curl -fsSL -o libvterm.tar.gz "https://launchpad.net/libvterm/trunk/v0.3/+download/libvterm-$version.tar.gz"
echo "$sha256  libvterm.tar.gz" | sha256sum -c -
tar xzf libvterm.tar.gz
cd "libvterm-$version"
make -s PREFIX=/usr/local
make -s install PREFIX=/usr/local
