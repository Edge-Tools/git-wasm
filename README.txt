git-wasm — Corresponding Source mirror
==================================================

This repository publishes the Corresponding Source for the WebAssembly
build of git (license: GPL-2.0-only) used in edgetools.io.

Contents
  build/      our build recipe: Dockerfile + helper scripts/config/patches.
              Rebuild with:  docker build build/
  upstream/   the exact upstream source archive(s) the build fetched,
              byte-identical and sha256-verified (see below).

Upstream sources:
  libgit2.tar.gz
    https://github.com/libgit2/libgit2/archive/refs/tags/v1.9.3.tar.gz
    sha256 d532172d7ab24d2a25944e2434212d63ee85f3650e97b5f7579e7f201a78ad64
