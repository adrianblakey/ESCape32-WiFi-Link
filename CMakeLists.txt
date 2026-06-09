cmake_minimum_required(VERSION 3.16)
# Version scheme: <upstream-base>-aart.<aart-revision>
# Tracks neoxic/ESCape32-WiFi-Link upstream; increment aart.N for AART-only changes.
set(PROJECT_VER "1.8-aart.1")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(aart-remora-programmer)
