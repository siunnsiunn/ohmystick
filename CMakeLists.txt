# Top-level CMake for ESP-IDF.
cmake_minimum_required(VERSION 3.16)
# Tailscale client for the WIFI app (MIT, pinned submodule; see docs/WIFI-PLAN.md).
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/third_party/microlink/components/microlink"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/microlink/components/microlink/components/wireguard_lwip")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "v0.1.6")
project(voxstick)
