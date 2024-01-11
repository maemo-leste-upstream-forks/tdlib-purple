#!/bin/bash
set -e

rm -rf build
mkdir build
pushd build
  git clone https://github.com/tdlib/td.git
  pushd td
    git checkout 1d1bc07eded7d3ee7df7c567e008bbf926cba081 # spoilers
    mkdir build
    pushd build
      cmake -DCMAKE_BUILD_TYPE=Release ..
      make
      make install DESTDIR=destdir
    popd
  popd
  cmake -DCMAKE_INSTALL_PREFIX:PATH=/usr -DTd_DIR="$(realpath .)"/td/build/destdir/usr/local/lib/cmake/Td/ -DNoVoip=True ..
  make
popd
