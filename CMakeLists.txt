# Top-level CMake for ESP-IDF.
cmake_minimum_required(VERSION 3.16)
# The WIFI app talks to winvoice over lwIP sockets on the LAN (docs/WIFI-PLAN.md; the Tailscale
# client tried in stage T was dropped).
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "v0.1.6")
project(voxstick)
