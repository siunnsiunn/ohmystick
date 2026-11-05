# Top-level CMake for ESP-IDF.
cmake_minimum_required(VERSION 3.16)
# The WIFI app talks plain lwIP sockets on the LAN; the Tailscale client (third_party/microlink,
# stage T of docs/WIFI-PLAN.md) is no longer part of the build.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "v0.1.6")
project(voxstick)
