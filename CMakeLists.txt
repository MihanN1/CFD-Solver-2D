cmake_minimum_required(VERSION 3.23)

project(CFD_Solver_2D
    VERSION 0.1.0
    LANGUAGES CXX
)

# ============================================================
# BASE SETTINGS
# ============================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep output predictable.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Project local folders.
set(CFD_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(CFD_LIB_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/lib")
set(CFD_MODELS_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/models")

file(MAKE_DIRECTORY "${CFD_INCLUDE_DIR}")
file(MAKE_DIRECTORY "${CFD_LIB_DIR}")
file(MAKE_DIRECTORY "${CFD_MODELS_DIR}")

include(FetchContent)
include(GNUInstallDirs)

# ============================================================
# DOWNLOAD HELPER
# ============================================================

function(cfd_download_if_missing NAME URL OUT_FILE)
    if(EXISTS "${OUT_FILE}")
        message(STATUS "${NAME}: found -> ${OUT_FILE}")
        return()
    endif()

    get_filename_component(OUT_DIR "${OUT_FILE}" DIRECTORY)
    file(MAKE_DIRECTORY "${OUT_DIR}")

    message(STATUS "${NAME}: missing, downloading...")
    message(STATUS "${NAME}: ${URL}")
    message(STATUS "${NAME}: -> ${OUT_FILE}")

    file(
        DOWNLOAD "${URL}" "${OUT_FILE}"
        SHOW_PROGRESS
        STATUS DOWNLOAD_STATUS
        TLS_VERIFY ON
    )

    list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
    list(GET DOWNLOAD_STATUS 1 STATUS_MESSAGE)

    if(NOT STATUS_CODE EQUAL 0)
        file(REMOVE "${OUT_FILE}")
        message(FATAL_ERROR "${NAME}: download failed: ${STATUS_MESSAGE}")
    endif()

    message(STATUS "${NAME}: downloaded")
endfunction()

# ============================================================
# OBJ - tiny_obj_loader
# goes into: include/tiny_obj_loader.h
# ============================================================

set(TINYOBJ_HEADER "${CFD_INCLUDE_DIR}/tiny_obj_loader.h")

cfd_download_if_missing(
    "tiny_obj_loader"
    "https://raw.githubusercontent.com/tinyobjloader/tinyobjloader/release/tiny_obj_loader.h"
    "${TINYOBJ_HEADER}"
)

# ============================================================
# STL - stl_reader
# goes into: lib/stl_reader/stl_reader.h
# ============================================================

set(STL_READER_DIR "${CFD_LIB_DIR}/stl_reader")
set(STL_READER_HEADER "${STL_READER_DIR}/stl_reader.h")

cfd_download_if_missing(
    "stl_reader"
    "https://raw.githubusercontent.com/sreiter/stl_reader/master/stl_reader.h"
    "${STL_READER_HEADER}"
)

# ============================================================
# SFML
# goes into: lib/sfml
# CMake downloads source there if missing.
# It is then built together with this project.
# ============================================================

set(SFML_LOCAL_DIR "${CFD_LIB_DIR}/sfml")

# Build SFML static to avoid DLL copying problems at start.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# Disable unused modules/build parts.
set(SFML_BUILD_AUDIO OFF CACHE BOOL "" FORCE)
set(SFML_BUILD_NETWORK OFF CACHE BOOL "" FORCE)
set(SFML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SFML_BUILD_DOC OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    SFML
    GIT_REPOSITORY https://github.com/SFML/SFML.git
    GIT_TAG 3.1.0
    SOURCE_DIR "${SFML_LOCAL_DIR}"
)

FetchContent_MakeAvailable(SFML)

# ============================================================
# PROGRAM
# ============================================================

add_executable(cfd_app
    src/main.cpp
    src/tiny_obj_loader_impl.cpp
)

target_include_directories(cfd_app
    PRIVATE
        "${CFD_INCLUDE_DIR}"
        "${STL_READER_DIR}"
)

target_link_libraries(cfd_app
    PRIVATE
        SFML::Graphics
        SFML::Window
        SFML::System
)

target_compile_definitions(cfd_app
    PRIVATE
        CFD_MODELS_DIR="${CFD_MODELS_DIR}"
)

# ============================================================
# INSTALL
# ============================================================

install(
    TARGETS cfd_app
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(
    DIRECTORY "${CFD_INCLUDE_DIR}/"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    DIRECTORY "${STL_READER_DIR}/"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/stl_reader
)

install(
    DIRECTORY "${CFD_MODELS_DIR}/"
    DESTINATION models
)
