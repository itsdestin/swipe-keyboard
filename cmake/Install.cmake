# Kept separately so staging contracts can exercise installation without compiling Qt.
include(GNUInstallDirs)
if(NOT DEFINED SWIPEKB_SOURCE)
    set(SWIPEKB_SOURCE "${CMAKE_SOURCE_DIR}")
endif()
set(_runtime "${CMAKE_INSTALL_DATADIR}/swipe-keyboard")
# WHY executable mode on ELF libraries: RPM's find-debuginfo scans executable files;
# a generic DIRECTORY copy otherwise makes every .so mode 0644 and emits an empty debugsource package.
install(DIRECTORY "${SWIPEKB_QML_DIR}/SwipeKeyboard" DESTINATION "${_runtime}/qml"
    FILES_MATCHING PATTERN "*.so*"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    PATTERN "qmldir" PATTERN "*.qml" PATTERN "*.qmltypes")
install(DIRECTORY "${SWIPEKB_ASSETS}/models/" DESTINATION "${_runtime}/models")
install(FILES "${SWIPEKB_ASSETS}/dict/en_swipe.combined" DESTINATION "${_runtime}/dict")
# WHY required, not OPTIONAL: a normal installation must contain every selectable language pack.
install(DIRECTORY "${SWIPEKB_ASSETS}/language-packs/" DESTINATION "${_runtime}/language-packs")
install(DIRECTORY "${SWIPEKB_ASSETS}/licenses/" DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/swipe-keyboard")
install(FILES "${SWIPEKB_SOURCE}/LICENSE" "${SWIPEKB_SOURCE}/packaging/assets.lock.json"
    "${SWIPEKB_SOURCE}/packaging/NOTICE" "${SWIPEKB_SOURCE}/packaging/language-models/TATOEBA-CC-BY-2.0-FR.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/swipe-keyboard")
install(FILES "${SWIPEKB_SOURCE}/packaging/SOURCE-PROVENANCE.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/swipe-keyboard" OPTIONAL)
install(FILES "${SWIPEKB_SOURCE}/integration/setup.py" "${SWIPEKB_SOURCE}/integration/layout.py" "${SWIPEKB_SOURCE}/integration/learning.py"
    "${SWIPEKB_SOURCE}/integration/swipe-keyboard.desktop" DESTINATION "${_runtime}/integration")
install(PROGRAMS "${SWIPEKB_SOURCE}/integration/swipe-keyboard" DESTINATION "${_runtime}/integration")
install(PROGRAMS "${SWIPEKB_SOURCE}/integration/swipe-keyboard" DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "${SWIPEKB_SOURCE}/integration/swipe-keyboard.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
# WHY the source contract is authoritative even for unstamped developer builds.
file(READ "${SWIPEKB_SOURCE}/packaging/release-targets.json" _release_contract)
string(JSON _release_version GET "${_release_contract}" version)
string(JSON _release_number GET "${_release_contract}" release)
set(SWIPEKB_PACKAGE_VERSION "${_release_version}-${_release_number}" CACHE STRING "Native package version for support diagnostics")
install(FILES "${SWIPEKB_SOURCE}/packaging/source-identity.json" DESTINATION "${_runtime}" OPTIONAL)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/runtime.json"
    "{\"qt\": \"${Qt6Core_VERSION_MAJOR}.${Qt6Core_VERSION_MINOR}\", \"package\": \"${SWIPEKB_PACKAGE_VERSION}\"}\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/runtime.json" DESTINATION "${_runtime}")
