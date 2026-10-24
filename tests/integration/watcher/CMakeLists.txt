# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.20)
project(selection_watcher LANGUAGES CXX)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core DBus)
find_package(KF6Config REQUIRED)
add_executable(selection-watcher main.cpp)
target_link_libraries(selection-watcher PRIVATE Qt6::Core Qt6::DBus KF6::ConfigCore)
