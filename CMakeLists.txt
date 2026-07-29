cmake_minimum_required(VERSION 3.28)

project(CFD_Solver_2D
    VERSION 0.1.0
    LANGUAGES CXX
)

include(GNUInstallDirs)
include(FetchContent)
include(CheckLanguage)

option(CFD_ENABLE_OPENMP "Enable OpenMP parallel loops when available" ON)
option(CFD_ENABLE_AVX2 "Compile CPU C++ sources with AVX2 support" ON)

if(WIN32)
    set(CFD_ENABLE_SFML_DEFAULT ON)
else()
    set(CFD_ENABLE_SFML_DEFAULT OFF)
endif()
option(CFD_ENABLE_SFML "Build and link the bundled SFML visualization dependency" ${CFD_ENABLE_SFML_DEFAULT})

set(CFD_CUDA_ARCHITECTURES "75" CACHE STRING
    "CUDA architectures. 75 targets the NVIDIA GTX 1660 Ti/Turing GPU."
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CUDA is required. Enabling it after project() allows a clear diagnostic when
# the toolkit or Visual Studio CUDA integration is missing.
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES "${CFD_CUDA_ARCHITECTURES}" CACHE STRING
        "CUDA architectures used by all CUDA targets."
    )
endif()

check_language(CUDA)
if(NOT CMAKE_CUDA_COMPILER)
    message(FATAL_ERROR
        "CUDA compiler detection failed. This project requires CUDA.\n"
        "For Visual Studio 2026, install CUDA Toolkit 13.2 or newer and select "
        "the Visual Studio Integration component.\n"
        "Then delete the build directory and configure again.\n"
        "CUDA_PATH='$ENV{CUDA_PATH}'"
    )
endif()

enable_language(CUDA)
find_package(CUDAToolkit 13.2 REQUIRED)

set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_EXTENSIONS OFF)

if(MSVC AND MSVC_VERSION GREATER_EQUAL 1950 AND CUDAToolkit_VERSION VERSION_LESS 13.2)
    message(FATAL_ERROR
        "Visual Studio 2026 requires CUDA Toolkit 13.2 or newer. "
        "Detected CUDA ${CUDAToolkit_VERSION}."
    )
endif()

set(CFD_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(CFD_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib")
set(CFD_MODELS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/models")
set(CFD_SFML_DIR "${CFD_LIB_DIR}/sfml")
set(CFD_STL_READER_DIR "${CFD_LIB_DIR}/stl_reader")
set(CFD_TINYOBJ_HEADER "${CFD_INCLUDE_DIR}/tiny_obj_loader.h")

# Keep generated files outside the source tree.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" config_upper)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/bin/${config}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/lib/${config}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/lib/${config}")
endforeach()

# tinyobjloader is a header-only dependency in this project.
if(NOT EXISTS "${CFD_TINYOBJ_HEADER}")
    message(FATAL_ERROR
        "Missing dependency: ${CFD_TINYOBJ_HEADER}\n"
        "Restore include/tiny_obj_loader.h before configuring the project."
    )
endif()

add_library(cfd_tinyobj INTERFACE)
target_include_directories(cfd_tinyobj INTERFACE "${CFD_INCLUDE_DIR}")

# stl_reader is header-only. Prefer the copy included in the project.
if(EXISTS "${CFD_STL_READER_DIR}/include/stl_reader/stl_reader.h")
    add_library(cfd_stl_reader INTERFACE)
    target_include_directories(cfd_stl_reader INTERFACE "${CFD_STL_READER_DIR}/include")
else()
    FetchContent_Declare(
        stl_reader
        URL https://github.com/sreiter/stl_reader/archive/refs/tags/v2.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(stl_reader)
    add_library(cfd_stl_reader ALIAS stl_reader)
endif()

# Build only the SFML modules used by the project. Prefer the included source copy.
if(CFD_ENABLE_SFML)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static dependencies" FORCE)
    set(SFML_BUILD_AUDIO OFF CACHE BOOL "" FORCE)
    set(SFML_BUILD_NETWORK OFF CACHE BOOL "" FORCE)
    set(SFML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SFML_BUILD_DOC OFF CACHE BOOL "" FORCE)
    set(SFML_BUILD_TEST_SUITE OFF CACHE BOOL "" FORCE)
    set(SFML_CONFIGURE_EXTRAS OFF CACHE BOOL "" FORCE)
    set(SFML_INSTALL_PKGCONFIG_FILES OFF CACHE BOOL "" FORCE)
    set(SFML_USE_SYSTEM_DEPS OFF CACHE BOOL "" FORCE)

    if(EXISTS "${CFD_SFML_DIR}/CMakeLists.txt")
        add_subdirectory(
            "${CFD_SFML_DIR}"
            "${CMAKE_BINARY_DIR}/_deps/sfml-build"
            EXCLUDE_FROM_ALL
        )
    else()
        FetchContent_Declare(
            SFML
            URL https://github.com/SFML/SFML/archive/refs/tags/3.1.0.zip
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_MakeAvailable(SFML)
    endif()
endif()

set(CFD_SOURCES
    src/main.cpp
    src/tiny_obj_loader_impl.cpp
    src/Config.cpp
    src/Mesh.cpp
    src/Solver.cpp
    src/Multigrid.cpp
    src/MultigridCuda.cu
)

add_executable(cfd_app ${CFD_SOURCES})

set_target_properties(cfd_app PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CUDA_STANDARD 17
    CUDA_STANDARD_REQUIRED ON
    CUDA_EXTENSIONS OFF
    CUDA_ARCHITECTURES "${CFD_CUDA_ARCHITECTURES}"
)

target_include_directories(cfd_app PRIVATE "${CFD_INCLUDE_DIR}")

target_link_libraries(cfd_app
    PRIVATE
        cfd_tinyobj
        cfd_stl_reader
        CUDA::cudart
)

if(CFD_ENABLE_SFML)
    target_link_libraries(cfd_app
        PRIVATE
            SFML::Graphics
            SFML::Window
            SFML::System
    )
endif()

target_compile_definitions(cfd_app
    PRIVATE
        USE_CUDA=1
        CFD_MODELS_DIR="${CFD_MODELS_DIR}"
        NOMINMAX
)

# CUDA 13.x CCCL requires MSVC's standards-conforming preprocessor.
# For .cu files, nvcc must forward the option to its MSVC host compiler.
if(MSVC)
    target_compile_options(cfd_app
        PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:/Zc:preprocessor>
            $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/Zc:preprocessor>
    )
endif()

if(CFD_ENABLE_OPENMP)
    find_package(OpenMP QUIET COMPONENTS CXX)
    if(OpenMP_CXX_FOUND)
        target_link_libraries(cfd_app PRIVATE OpenMP::OpenMP_CXX)
        message(STATUS "OpenMP: enabled")
    else()
        message(STATUS "OpenMP: not found; building without OpenMP")
    endif()
endif()

if(CFD_ENABLE_AVX2)
    if(MSVC)
        target_compile_options(cfd_app PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/arch:AVX2>)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(cfd_app PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-mavx2>)
    else()
        message(WARNING "AVX2 flags are not configured for ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

install(
    TARGETS cfd_app
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(
    DIRECTORY "${CFD_INCLUDE_DIR}/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

if(EXISTS "${CFD_STL_READER_DIR}/include/")
    install(
        DIRECTORY "${CFD_STL_READER_DIR}/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
endif()

install(
    DIRECTORY "${CFD_MODELS_DIR}/"
    DESTINATION models
)

message(STATUS "CFD Solver configuration")
message(STATUS "  CUDA:         required")
message(STATUS "  CUDA toolkit: ${CUDAToolkit_VERSION}")
message(STATUS "  CUDA arch:    ${CFD_CUDA_ARCHITECTURES}")
message(STATUS "  SFML:         ${CFD_ENABLE_SFML}")
message(STATUS "  OpenMP:       ${CFD_ENABLE_OPENMP}")
message(STATUS "  AVX2:         ${CFD_ENABLE_AVX2}")
message(STATUS "  Build dir:    ${CMAKE_BINARY_DIR}")
