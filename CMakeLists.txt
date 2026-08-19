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
option(CFD_ENABLE_CUDA "Enable the CUDA multigrid backend" ON)
option(CFD_ENABLE_CUDA_EXPLICIT "Treat a missing CUDA toolkit as a hard error" OFF)
option(CFD_ENABLE_OPENMP_EXPLICIT "Treat a missing OpenMP runtime as a hard error" OFF)

# AppleClang carries no OpenMP runtime of its own, so libomp has to be pointed
# at: Homebrew's prefix for a native build, an unpacked bottle for the other
# architecture when one Mac builds both. The prefix must hold include/omp.h and
# lib/libomp.a or lib/libomp.dylib. Empty means "let find_package look", which
# on macOS finds nothing and produces an OpenMP-less binary under an OpenMP
# name, so the release script never leaves it empty for an OpenMP row.
set(CFD_OPENMP_ROOT "" CACHE PATH
    "Prefix of the libomp to build against (macOS)"
)

set(CFD_APP_NAME "Fluid Solver" CACHE STRING
    "File name of the produced executable, without the extension"
)

# Release builds ship as one file. What that costs differs per platform, and
# getting it wrong is quiet rather than loud, so it lives here instead of in
# whatever command line happened to produce a given binary:
#   MSVC       static CRT (/MT). The OpenMP runtime has no static form, so an
#              OpenMP build still needs vcomp140.dll beside it.
#   GCC/Clang  -static, plus libgomp.a rather than the libgomp.so that CMake's
#              OpenMP::OpenMP_CXX target otherwise hands the linker.
#   with CUDA  no full -static: libcudart_static needs dlopen and pthread, and
#              the driver is dynamic by definition. The C++ runtime is still
#              linked in, which leaves libc, libm and libcuda.so.1.
#   macOS      there is no static libSystem, so this only means no third-party
#              dylibs. That is as static as an Apple binary is allowed to be.
option(CFD_STATIC "Link the runtime into the executable" ON)

if(CFD_STATIC AND MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(
    CFD_RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/bin"
    CACHE PATH "Root directory for the generated cfd_app executable"
)

# A released binary has to run on more than the machine that built it. CMake
# emits SASS for each of these plus PTX for the last, so newer cards JIT and
# work; without the older entries nothing before Turing runs at all. CUDA 13
# dropped Maxwell, Pascal and Volta, so building the release rows against
# CUDA 12.x is what keeps 50..70 reachable - a 13.x toolkit will refuse them
# and the list has to be trimmed to 75 and up.
set(CFD_CUDA_ARCHITECTURES "50;60;61;70;75;80;86;89;90" CACHE STRING
    "CUDA architectures to generate code for."
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CFD_ENABLE_CUDA)
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        set(CMAKE_CUDA_ARCHITECTURES "${CFD_CUDA_ARCHITECTURES}" CACHE STRING
            "CUDA architectures used by all CUDA targets."
        )
    endif()

    check_language(CUDA)
    if(NOT CMAKE_CUDA_COMPILER)
        # CFD_ENABLE_CUDA defaults to ON, so a machine without a toolkit used to
        # fail to configure at all. Fall back to the CPU backend instead and say
        # so; only an explicit request is treated as an error.
        if(CFD_ENABLE_CUDA_EXPLICIT)
            message(FATAL_ERROR
                "CUDA compiler detection failed while CFD_ENABLE_CUDA=ON.\n"
                "CUDA_PATH='$ENV{CUDA_PATH}'"
            )
        endif()
        message(WARNING
            "No CUDA compiler found; building the CPU backend only. "
            "Install the CUDA Toolkit (with the Visual Studio Integration "
            "component on Windows) and reconfigure in a fresh build directory "
            "to enable the GPU path. CUDA_PATH='$ENV{CUDA_PATH}'"
        )
        set(CFD_ENABLE_CUDA OFF)
    endif()
endif()

if(CFD_ENABLE_CUDA)
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)

    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
    set(CMAKE_CUDA_EXTENSIONS OFF)

    if(MSVC AND MSVC_VERSION GREATER_EQUAL 1950 AND CUDAToolkit_VERSION VERSION_LESS "13.2")
        message(FATAL_ERROR
            "Visual Studio 2026 requires CUDA Toolkit 13.2 or newer. "
            "Detected CUDA ${CUDAToolkit_VERSION}."
        )
    endif()
endif()

set(CFD_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(CFD_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib")
set(CFD_MODELS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/models")
set(CFD_STL_READER_DIR "${CFD_LIB_DIR}/stl_reader")
set(CFD_TINYOBJ_HEADER "${CFD_INCLUDE_DIR}/tiny_obj_loader.h")

# Keep generated files outside the source tree.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CFD_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" config_upper)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${CFD_RUNTIME_OUTPUT_DIRECTORY}/${config}")
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

set(CFD_SOURCES
    src/main.cpp
    src/tiny_obj_loader_impl.cpp
    src/AppPaths.cpp
    src/Config.cpp
    src/Mesh.cpp
    src/Solver.cpp
    src/Multigrid.cpp
)

if(CFD_ENABLE_CUDA)
    list(APPEND CFD_SOURCES src/MultigridCuda.cu)
endif()

# The icon and the version block live in a resource script, which is a Windows
# concept only. Elsewhere the icon belongs to the .desktop file or the app
# bundle instead, so there is nothing to compile in.
option(CFD_ENABLE_ICON "Embed the icon and version block on Windows" ON)

if(WIN32 AND CFD_ENABLE_ICON)
    enable_language(RC)
    set(CFD_ICON_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/logo/fluid-solver.ico")
    if(EXISTS "${CFD_ICON_SOURCE}")
        # rc.exe reads the .rc file in the system ANSI code page, not UTF-8,
        # so an absolute path with non-ASCII characters anywhere in it - and
        # this project lives under one - arrives as mojibake and the icon
        # "does not exist". Nothing in the .rc may be non-ASCII. So the icon is
        # copied next to the generated .rc and referenced by a bare name, which
        # rc.exe resolves relative to the .rc itself.
        file(COPY "${CFD_ICON_SOURCE}" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
        set(CFD_ICON_PATH "fluid-solver.ico")
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/src/app.rc.in"
            "${CMAKE_CURRENT_BINARY_DIR}/app.rc"
            @ONLY
        )
        list(APPEND CFD_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
    else()
        message(WARNING
            "logo/fluid-solver.ico is missing; the executable gets the default "
            "Windows icon and no version information."
        )
    endif()
endif()

add_executable(cfd_app ${CFD_SOURCES})

add_custom_command(
    TARGET cfd_app
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E echo
        "Built executable: $<TARGET_FILE:cfd_app>"
    VERBATIM
)

set_target_properties(cfd_app PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    # The target keeps its old name so the GUI project, which looks for
    # cfd_app, still resolves; only the file on disk is renamed. There will be
    # other solvers next to this one, and "cfd_app" says nothing about which.
    OUTPUT_NAME "${CFD_APP_NAME}"
)

if(CFD_ENABLE_CUDA)
    set_target_properties(cfd_app PROPERTIES
        CUDA_STANDARD 17
        CUDA_STANDARD_REQUIRED ON
        CUDA_EXTENSIONS OFF
        CUDA_ARCHITECTURES "${CFD_CUDA_ARCHITECTURES}"
    )
endif()

target_include_directories(cfd_app PRIVATE "${CFD_INCLUDE_DIR}")

target_link_libraries(cfd_app
    PRIVATE
        cfd_tinyobj
        cfd_stl_reader
)

if(CFD_ENABLE_CUDA)
    if(CFD_STATIC)
        target_link_libraries(cfd_app PRIVATE CUDA::cudart_static)
    else()
        target_link_libraries(cfd_app PRIVATE CUDA::cudart)
    endif()
endif()

target_compile_definitions(cfd_app
    PRIVATE
        CFD_MODELS_DIR="${CFD_MODELS_DIR}"
        NOMINMAX
)

if(CFD_ENABLE_CUDA)
    target_compile_definitions(cfd_app PRIVATE USE_CUDA=1)
endif()

# CUDA 13.x CCCL requires MSVC's standards-conforming preprocessor.
# For .cu files, nvcc must forward the option to its MSVC host compiler.
if(MSVC)
    target_compile_options(cfd_app
        PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:/Zc:preprocessor>
    )
    if(CFD_ENABLE_CUDA)
        target_compile_options(cfd_app
            PRIVATE
                $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/Zc:preprocessor>
        )
    endif()
endif()

if(CFD_ENABLE_OPENMP)
    # CMake's OpenMP::OpenMP_CXX records the path of the *shared* libgomp, and
    # -static then dies with "attempted static link of dynamic object". Point
    # the search at the archive before find_package runs, and the imported
    # target comes out static without anything downstream noticing.
    if(CFD_STATIC AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" ${CMAKE_CXX_FLAGS} -print-file-name=libgomp.a
            OUTPUT_VARIABLE CFD_LIBGOMP_A
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(EXISTS "${CFD_LIBGOMP_A}")
            set(OpenMP_CXX_FLAGS "-fopenmp")
            set(OpenMP_CXX_LIB_NAMES "gomp")
            set(OpenMP_gomp_LIBRARY "${CFD_LIBGOMP_A}")
            message(STATUS "OpenMP: static ${CFD_LIBGOMP_A}")
        endif()
    endif()

    # Same idea on macOS, one level further down: there is no OpenMP in the
    # compiler at all, so the flags, the library name and the library itself are
    # all supplied here and find_package only verifies them. The static archive
    # wins when it is there, or the binary stops working the moment the machine
    # running it has no Homebrew.
    if(APPLE AND CFD_OPENMP_ROOT AND EXISTS "${CFD_OPENMP_ROOT}/include/omp.h")
        if(CFD_STATIC AND EXISTS "${CFD_OPENMP_ROOT}/lib/libomp.a")
            set(OpenMP_omp_LIBRARY "${CFD_OPENMP_ROOT}/lib/libomp.a")
        elseif(EXISTS "${CFD_OPENMP_ROOT}/lib/libomp.dylib")
            set(OpenMP_omp_LIBRARY "${CFD_OPENMP_ROOT}/lib/libomp.dylib")
        endif()
        if(OpenMP_omp_LIBRARY)
            set(OpenMP_CXX_FLAGS "-Xclang -fopenmp -I${CFD_OPENMP_ROOT}/include")
            set(OpenMP_CXX_LIB_NAMES "omp")
            message(STATUS "OpenMP: ${OpenMP_omp_LIBRARY}")
        endif()
    endif()

    find_package(OpenMP QUIET COMPONENTS CXX)
    if(OpenMP_CXX_FOUND)
        target_link_libraries(cfd_app PRIVATE OpenMP::OpenMP_CXX)
        message(STATUS "OpenMP: enabled")
    else()
        # Same reason as the CUDA one: silently dropping OpenMP here would
        # publish a binary named for a feature it does not have.
        if(CFD_ENABLE_OPENMP_EXPLICIT)
            message(FATAL_ERROR
                "OpenMP was requested with CFD_ENABLE_OPENMP_EXPLICIT=ON but no "
                "runtime was found. On macOS install libomp (brew install libomp) "
                "and pass -DCFD_OPENMP_ROOT= the prefix brew --prefix libomp prints."
            )
        endif()
        message(STATUS "OpenMP: not found; building without OpenMP")
    endif()
endif()

# Everything except MSVC, which was handled by CMAKE_MSVC_RUNTIME_LIBRARY above.
if(CFD_STATIC AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    if(APPLE)
        # No static libSystem exists; only the C++ runtime can be pulled in.
        target_link_options(cfd_app PRIVATE -static-libstdc++)
    elseif(CFD_ENABLE_CUDA)
        # libcudart_static wants dlopen and pthread, so the executable stays
        # dynamically linked against libc and the driver.
        target_link_options(cfd_app PRIVATE -static-libgcc -static-libstdc++)
    else()
        target_link_options(cfd_app PRIVATE
            -static -static-libgcc -static-libstdc++)
        if(CFD_ENABLE_OPENMP)
            target_link_options(cfd_app PRIVATE
                -Wl,--whole-archive -lpthread -ldl -Wl,--no-whole-archive)
        endif()
    endif()
endif()

if(CFD_ENABLE_AVX2)
    if(MSVC)
        target_compile_options(cfd_app PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/arch:AVX2>)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # /arch:AVX2 already implies FMA on MSVC; -mavx2 alone does not on
        # GCC/Clang, and the predictor contracts into FMA once it is allowed.
        target_compile_options(cfd_app PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-mavx2 -mfma>)
    else()
        message(WARNING "AVX2 flags are not configured for ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

include(CTest)


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
message(STATUS "  CUDA:         ${CFD_ENABLE_CUDA}")
if(CFD_ENABLE_CUDA)
    message(STATUS "  CUDA toolkit: ${CUDAToolkit_VERSION}")
    message(STATUS "  CUDA arch:    ${CFD_CUDA_ARCHITECTURES}")
endif()
message(STATUS "  OpenMP:       ${CFD_ENABLE_OPENMP}")
message(STATUS "  AVX2:         ${CFD_ENABLE_AVX2}")
message(STATUS "  Build dir:    ${CMAKE_BINARY_DIR}")
