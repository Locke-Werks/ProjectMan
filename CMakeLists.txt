cmake_minimum_required(VERSION 3.28)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Version)

project(ProjectMan VERSION ${PM_VERSION} LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "ProjectMan targets Windows only.")
endif()

# x64 only, matching Forge and Docked Console. It removes WOW64 registry and
# Program Files redirection from the problem entirely.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ProjectMan is x64 only. Configure with an x64 toolchain.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(PM_WERROR    "Treat warnings as errors"                           OFF)
option(PM_HARDENING "Apply exploit-mitigation compiler and linker flags"  ON)
option(PM_BUILD_GUI "Build ProjectMan.exe, the Qt desktop dashboard"      ON)
option(PM_BUILD_CLI "Build pm.exe, the console picker"                    ON)

include(Dependencies)
include(Warnings)
include(Hardening)
include(Manifest)
include(DeployQt)

# The CRT is deliberately left at the default dynamic /MD. Docked Console links
# the static CRT because it ships one self-contained exe; ProjectMan.exe loads
# the official Qt binaries, which are themselves built against /MD, and mixing
# the two produces a duplicate heap and LNK2038.

# Qt is a GUI-only dependency. src/core is deliberately Qt-free, which is what
# lets pm.exe ship as a single standalone binary rather than needing
# Qt6Core.dll beside it on PATH.
if(PM_BUILD_GUI)
    find_package(Qt6 6.5 QUIET COMPONENTS Core Gui Widgets)
    if(NOT Qt6_FOUND)
        message(FATAL_ERROR
            "Qt6 Widgets not found. Point CMake at a Qt kit with "
            "-DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64, or set the "
            "QT_ROOT_DIR environment variable, or configure with "
            "-DPM_BUILD_GUI=OFF to build only the console front end.")
    endif()
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
endif()

# RC wants forward slashes or doubled backslashes in a path literal. CMake hands
# out forward slashes, so pass the path through rather than composing one in the
# template.
set(PM_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/projectman.ico")

# Captured at top level. Inside the function these would resolve against
# whichever src/ directory called it, not the project root.
set(PM_RC_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/res/projectman.rc.in")
set(PM_MANIFEST    "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")

# Configure the shared VERSIONINFO template once per binary. The three name
# fields differ; every version field comes from cmake/Version.cmake.
function(pm_configure_rc out_var internal_name original_filename description)
    set(PM_RC_INTERNAL_NAME     "${internal_name}")
    set(PM_RC_ORIGINAL_FILENAME "${original_filename}")
    set(PM_RC_DESCRIPTION       "${description}")
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/${internal_name}.rc")
    configure_file(
        "${PM_RC_TEMPLATE}"
        "${_out}"
        @ONLY
    )
    set(${out_var} "${_out}" PARENT_SCOPE)
endfunction()

add_subdirectory(src/core)

if(PM_BUILD_CLI)
    add_subdirectory(src/cli)
endif()

if(PM_BUILD_GUI)
    add_subdirectory(src/gui)
endif()

message(STATUS "")
message(STATUS "ProjectMan ${PM_VERSION}")
if(PM_BUILD_GUI)
    message(STATUS "  Qt        : ${Qt6_VERSION} (${Qt6_DIR})")
endif()
message(STATUS "  CLI       : ${PM_BUILD_CLI}")
message(STATUS "  GUI       : ${PM_BUILD_GUI}")
message(STATUS "  hardening : ${PM_HARDENING}  werror: ${PM_WERROR}")
message(STATUS "")
