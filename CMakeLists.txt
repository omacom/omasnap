cmake_minimum_required(VERSION 3.24)
project(omasnap LANGUAGES C CXX)

# The release tag is the version: git describe in a checkout, or the directory
# name of a GitHub release tarball (omasnap-1.26.0). Pass -DOMASNAP_VERSION to
# override it.
set(OMASNAP_VERSION "" CACHE STRING "Version to report; derived from the release tag when empty")
if(NOT OMASNAP_VERSION AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" describe --tags --match "v[0-9]*"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      OUTPUT_VARIABLE OMASNAP_VERSION
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    string(REGEX REPLACE "^v" "" OMASNAP_VERSION "${OMASNAP_VERSION}")
  endif()
endif()
get_filename_component(omasnap_source_name "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
if(NOT OMASNAP_VERSION AND omasnap_source_name MATCHES "^omasnap-([0-9]+(\\.[0-9]+)*)$")
  set(OMASNAP_VERSION "${CMAKE_MATCH_1}")
endif()
if(NOT OMASNAP_VERSION)
  set(OMASNAP_VERSION dev)
endif()
message(STATUS "omasnap version: ${OMASNAP_VERSION}")
include(GNUInstallDirs)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
find_package(Qt6 6.8 REQUIRED COMPONENTS Concurrent Core Gui Test Widgets)
find_package(LayerShellQt REQUIRED)
pkg_check_modules(WaylandClient REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(Libdeflate REQUIRED IMPORTED_TARGET libdeflate)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

set(PROTOCOL_XMLS
  # ext-image-capture-source references this interface even for output-only use.
  /usr/share/wayland-protocols/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml
  /usr/share/wayland-protocols/staging/ext-image-capture-source/ext-image-capture-source-v1.xml
  /usr/share/wayland-protocols/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml
  ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-virtual-pointer-unstable-v1.xml
)
set(PROTOCOL_SOURCES)
foreach(PROTOCOL_XML IN LISTS PROTOCOL_XMLS)
  get_filename_component(PROTOCOL_NAME "${PROTOCOL_XML}" NAME_WE)
  set(PROTOCOL_HEADER "${CMAKE_CURRENT_BINARY_DIR}/${PROTOCOL_NAME}-client-protocol.h")
  set(PROTOCOL_CODE "${CMAKE_CURRENT_BINARY_DIR}/${PROTOCOL_NAME}-protocol.c")
  add_custom_command(
    OUTPUT "${PROTOCOL_HEADER}" "${PROTOCOL_CODE}"
    COMMAND "${WAYLAND_SCANNER}" client-header "${PROTOCOL_XML}" "${PROTOCOL_HEADER}"
    COMMAND "${WAYLAND_SCANNER}" private-code "${PROTOCOL_XML}" "${PROTOCOL_CODE}"
    DEPENDS "${PROTOCOL_XML}"
    VERBATIM
  )
  list(APPEND PROTOCOL_SOURCES "${PROTOCOL_HEADER}" "${PROTOCOL_CODE}")
endforeach()

# Everything but the entry points, shared by the app, the tests and
# stitch-replay, so each source compiles once and a new file in src/ needs no
# line here.
file(GLOB omasnap_sources CONFIGURE_DEPENDS src/*.cpp src/*.hpp)
list(REMOVE_ITEM omasnap_sources
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/stitch-replay.cpp)
add_library(omasnap_core STATIC ${omasnap_sources} ${PROTOCOL_SOURCES})
qt_add_resources(omasnap_core "capture-fonts"
  PREFIX "/fonts"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
  FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/Neucha.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/JetBrainsMono-Regular.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/InterDisplay-SemiBold.ttf"
)

# CXX-only: omasnap_core also compiles Wayland protocol .c files.
target_precompile_headers(omasnap_core PRIVATE
  "$<$<COMPILE_LANGUAGE:CXX>:<QtWidgets/QWidget$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<QtWidgets/QApplication$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<QtGui/QPainter$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<QtGui/QPainterPath$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<QtGui/QImage$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<QtGui/QPixmap$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<QtGui/QFontDatabase$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<QtGui/QMouseEvent$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<QtGui/QKeyEvent$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<QtGui/QWheelEvent$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<QtConcurrent/QtConcurrentRun$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<QtCore/QFutureWatcher$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<QtCore/QProcess$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<QtCore/QFile$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<QtCore/QDir$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<QtCore/QFileInfo$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<QtCore/QJsonObject$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<QtCore/QJsonArray$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<QtCore/QJsonDocument$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<QtCore/QTimer$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<QtCore/QString$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<QtCore/QVector$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<algorithm$<ANGLE-R>>"
  "$<$<COMPILE_LANGUAGE:CXX>:<cmath$<ANGLE-R>>"
)

target_include_directories(omasnap_core PUBLIC
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_features(omasnap_core PUBLIC cxx_std_23)
target_compile_options(omasnap_core PUBLIC -Wall -Wextra -Wpedantic)
target_link_libraries(omasnap_core PUBLIC
  Qt6::Concurrent
  Qt6::Core
  Qt6::Gui
  Qt6::Widgets
  LayerShellQt::Interface
  PkgConfig::WaylandClient
  PkgConfig::Libdeflate
)

qt_add_executable(omasnap src/main.cpp)
target_compile_definitions(omasnap PRIVATE OMASNAP_VERSION="${OMASNAP_VERSION}")
target_link_libraries(omasnap PRIVATE omasnap_core)
target_precompile_headers(omasnap REUSE_FROM omasnap_core)

enable_testing()
file(GLOB omasnap_test_sources CONFIGURE_DEPENDS tests/*.cpp tests/*.hpp)
add_executable(omasnap_tests ${omasnap_test_sources})
target_link_libraries(omasnap_tests PRIVATE omasnap_core Qt6::Test)
target_precompile_headers(omasnap_tests REUSE_FROM omasnap_core)
add_test(NAME omasnap_tests COMMAND omasnap_tests)
set_tests_properties(omasnap_tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

# Static analysis with the checks in .clang-tidy, where any finding fails.
# Qt adds a GCC-only flag that clang rejects, so it is dropped for the analysis.
find_program(RUN_CLANG_TIDY run-clang-tidy)
if(RUN_CLANG_TIDY)
  add_test(NAME omasnap_lint
    COMMAND "${RUN_CLANG_TIDY}" -p "${CMAKE_CURRENT_BINARY_DIR}" -quiet -hide-progress
            -removed-arg=-mno-direct-extern-access "/(src|tests)/[^/]+\\.cpp$")
endif()

# Replays a dumped frame directory through the stitcher, with no compositor.
add_executable(stitch-replay src/stitch-replay.cpp)
target_link_libraries(stitch-replay PRIVATE omasnap_core)

install(TARGETS omasnap
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(FILES assets/OFL.txt
  DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/omasnap"
  RENAME Neucha-OFL.txt
)
install(FILES assets/JetBrainsMono-OFL.txt assets/Inter-OFL.txt assets/Lucide-ISC.txt
  DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/omasnap"
)
install(FILES omasnap.desktop
  DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
)
install(FILES omasnap.svg
  DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
)
