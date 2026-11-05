#!/usr/bin/env bash
# Build the Raptor RDF parser for a wheel, which bundles it. Usage: build_raptor.sh <prefix>
set -euo pipefail
prefix="$1"
version=2.0.16
work="$(mktemp -d)"
curl -fsSL "https://download.librdf.org/source/raptor2-${version}.tar.gz" -o "$work/raptor.tar.gz"
tar -xzf "$work/raptor.tar.gz" -C "$work"
cd "$work/raptor2-${version}"
./configure --prefix="$prefix" --disable-gtk-doc --with-www=none --disable-static
make -j"$(getconf _NPROCESSORS_ONLN)"
make install
