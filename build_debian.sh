#!/bin/bash
set -e

  pushd td
    mkdir build
    pushd build
      cmake -DCMAKE_BUILD_TYPE=Release ..
      make
      make install DESTDIR=destdir
    popd
  popd

mkdir build
pushd build
  cmake -DCMAKE_INSTALL_PREFIX:PATH=/usr -DTd_DIR="$(realpath .)"../td/build/destdir/usr/local/lib/cmake/Td/ -DNoVoip=True ..
  make
popd
