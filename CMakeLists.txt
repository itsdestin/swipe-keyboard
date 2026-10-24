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
# WHY a cache path lets tests/builds select an explicit read-only fixture without changing the source tree's default.
set(SWIPEKB_ASSETS ${CMAKE_SOURCE_DIR}/assets CACHE PATH "Swipe keyboard model and language-pack assets")

enable_testing()
find_package(GTest REQUIRED)
add_subdirectory(engine)
add_subdirectory(plugin)
# Plasma 6.9's rebuilt keyboard: a second front end without Qt Virtual Keyboard or FUTO; the launcher picks one.
add_subdirectory(frontend-plasma69)
add_subdirectory(tools)
add_subdirectory(integration)
add_subdirectory(gaming/daemon)

# Capture the build Qt minor for the launcher's fail-closed ABI check.
find_package(Qt6 6.8 REQUIRED COMPONENTS Core)
include(cmake/Install.cmake)
