# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.29)
project(swipe_keyboard_vm_acceptance LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick DBus)
qt_standard_project_setup()
qt_add_executable(session-client session-client.cpp)
target_link_libraries(session-client PRIVATE Qt6::Quick Qt6::DBus)
