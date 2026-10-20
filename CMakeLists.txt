cmake_minimum_required(VERSION 3.29)
project(swipe_keyboard LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# WHY: everything ends up inside a Qt plugin (.so), so every static piece must be position-independent.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# The finished QML module (plugin + engine .so + qmldir) is assembled here; install.sh copies it.
set(SWIPEKB_QML_DIR ${CMAKE_BINARY_DIR}/qml)
set(SWIPEKB_ASSETS ${CMAKE_SOURCE_DIR}/assets)

# Distributable binaries must not inherit upstream's host-specific default.
set(USE_MARCH_NATIVE OFF CACHE BOOL "Enable host-specific instructions (developer only)")

# FUTO's library. ExecuTorch must already be built by scripts/build-futo.sh.
add_subdirectory(third_party/swipe-library EXCLUDE_FROM_ALL)
if(NOT TARGET executorch)
    message(FATAL_ERROR "ExecuTorch not built — run scripts/build-futo.sh first")
endif()

enable_testing()
find_package(GTest REQUIRED)
add_subdirectory(engine)
add_subdirectory(plugin)
add_subdirectory(tools)
add_subdirectory(integration)

# Capture the build Qt minor for the launcher's fail-closed ABI check.
find_package(Qt6 6.8 REQUIRED COMPONENTS Core)
include(cmake/Install.cmake)
