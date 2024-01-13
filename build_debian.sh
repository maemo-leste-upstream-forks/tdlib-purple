#!/bin/bash
set -e

# armhf runs out of address space it seems, with g++ 10, so let's try -O1 and
# see if it passes
export CFLAGS=-O1
export CXXFLAGS=-O1

  pushd td
    mkdir build
    pushd build
      cmake -DCMAKE_INSTALL_PREFIX:PATH=/usr -DCMAKE_BUILD_TYPE=Release ..
      make -j1
      make install DESTDIR=destdir
    popd
  popd

mkdir build
pushd build
  cmake -DCMAKE_INSTALL_PREFIX:PATH=/usr -DTd_DIR="$(realpath .)"/../td/build/destdir/usr/lib/cmake/Td/ -DNoVoip=True ..
  make -j1
popd
