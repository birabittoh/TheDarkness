# thedarkness - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(thedarkness LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Embedded window icon: extracted directly from assets/default.xex (already a
# required codegen input) so no extra asset needs to be checked in or fetched.
# src/icon.generated.h is a gitignored build artifact regenerated whenever the
# XEX or the generator script changes.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(THEDARKNESS_ICON_HEADER ${CMAKE_SOURCE_DIR}/src/icon.generated.h)
set(THEDARKNESS_ICON_ICO ${CMAKE_SOURCE_DIR}/src/icon.generated.ico)
add_custom_command(
    OUTPUT ${THEDARKNESS_ICON_HEADER} ${THEDARKNESS_ICON_ICO}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-icon.py
    DEPENDS
        ${CMAKE_SOURCE_DIR}/scripts/gen-icon.py
        ${CMAKE_SOURCE_DIR}/assets/default.xex
    COMMENT "Generating src/icon.generated.h and src/icon.generated.ico"
    VERBATIM
)

# On Windows, also embed the same icon as the .exe's own resource (Explorer,
# taskbar before the window opens, Alt-Tab), separate from the in-window icon
# applied at runtime via Window::SetIcon (see thedarkness_app.h). The .rc
# needs an absolute path to the .ico since it's not necessarily emitted next
# to it in an out-of-source build.
if(WIN32)
    set(THEDARKNESS_ICON_ICO_PATH ${THEDARKNESS_ICON_ICO})
    set(THEDARKNESS_ICON_RC ${CMAKE_SOURCE_DIR}/src/thedarkness.generated.rc)
    configure_file(
        ${CMAKE_SOURCE_DIR}/src/thedarkness.rc.in
        ${THEDARKNESS_ICON_RC}
        @ONLY
    )
    # rc.exe needs icon.generated.ico to exist by the time it compiles this
    # .rc; configure_file only stamped the path into it above, it doesn't
    # establish a build-order dependency on the custom command that (re)writes
    # the .ico itself.
    set_source_files_properties(${THEDARKNESS_ICON_RC} PROPERTIES OBJECT_DEPENDS ${THEDARKNESS_ICON_ICO})
endif()

# Sources
set(THEDARKNESS_SOURCES
    src/main.cpp
    src/hooks/camera_mouselook_hook.cpp
    src/xbox_live.cpp
    ${THEDARKNESS_ICON_HEADER}
)

if(WIN32)
    add_executable(thedarkness WIN32 ${THEDARKNESS_SOURCES} ${THEDARKNESS_ICON_RC})
else()
    add_executable(thedarkness ${THEDARKNESS_SOURCES})
endif()

rexglue_setup_target(thedarkness)
