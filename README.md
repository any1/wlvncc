# Wayland VNC Client

## Introduction
This is a work-in-progress implementation of a Wayland native VNC client.
Expect bugs and missing features.

## Runtime Dependencies
 * aml
 * libvncclient
 * libwayland
 * libxkbcommon
 * pixman

## Build Dependencies
 * GCC/clang
 * meson
 * ninja
 * pkg-config
 * wayland-protocols

## Building & Running
```
git clone https://github.com/any1/aml.git
git clone https://github.com/any1/wlvncc.git

mkdir wlvncc/subprojects
cd wlvncc/subprojects
ln -s ../../aml .
cd -

meson build
ninja -C build

./build/wlvncc <address>
```
