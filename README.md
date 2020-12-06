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
At the time of writing, an unreleased version of libvncclient is required. So,
you must either use bleeding edge git based packages for the project, or build
libvncclient as a subproject.

```
git clone https://github.com/any1/aml.git
git clone https://github.com/LibVNC/libvncserver.git
git clone https://github.com/any1/wlvncc.git

mkdir wlvncc/subprojects
cd wlvncc/subprojects
ln -s ../../aml .
ln -s ../../libvncserver .
cd -

meson build
ninja -C build

./build/wlvncc <address>
```
