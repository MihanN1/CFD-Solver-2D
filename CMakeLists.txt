cmake_minimum_required(VERSION 3.28)

project(CFDMaskUIOptimized
    VERSION 0.1.0
    LANGUAGES C CXX
)

include(FetchContent)
option(BUILD_TESTING "Build CFD Mask UI tests" OFF)
option(CFD_UI_ENABLE_AVX2 "Compile UI-owned code with AVX2 instructions" OFF)
option(CFD_UI_STATIC_RUNTIME "Prefer static C/C++ runtimes where the platform supports it" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Release binaries should carry as little redistributable runtime baggage as
# the platform permits. Windows can statically link the MSVC CRT. Linux keeps
# the system graphics/window stack dynamic but can carry libgcc/libstdc++.
# macOS does not support statically linking its system C++ runtime.
if(MSVC AND CFD_UI_STATIC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# The GUI is buildable without the CFD solver source tree.
# tinyobjloader is vendored into this GUI package because it is a direct
# source dependency of GeometryProcessor/tiny_obj_loader_impl.
set(CFD_TINYOBJ_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinyobjloader")
if(NOT EXISTS "${CFD_TINYOBJ_DIR}/tiny_obj_loader.h")
    message(FATAL_ERROR
        "GUI package is incomplete: ${CFD_TINYOBJ_DIR}/tiny_obj_loader.h is missing."
    )
endif()

# The solver executable is optional at build time. The running GUI can select
# Fluid Solver.exe later without rebuilding the GUI.
set(
    CFD_SOLVER_EXE
    ""
    CACHE FILEPATH "Optional path to Fluid Solver.exe to copy beside the GUI"
)
if(NOT CFD_SOLVER_EXE STREQUAL "")
    get_filename_component(CFD_SOLVER_EXE "${CFD_SOLVER_EXE}" ABSOLUTE)
endif()
if(NOT EXISTS "${CFD_SOLVER_EXE}")
    if(NOT CFD_SOLVER_EXE STREQUAL "")
        message(WARNING
            "CFD_SOLVER_EXE does not exist: ${CFD_SOLVER_EXE}. "
            "The GUI will still build; select Fluid Solver.exe at runtime."
        )
    else()
        message(STATUS
            "No solver EXE supplied. The GUI will build standalone and can "
            "select Fluid Solver.exe at runtime."
        )
    endif()
    set(CFD_SOLVER_EXE "")
endif()
file(TO_CMAKE_PATH "${CFD_SOLVER_EXE}" CFD_SOLVER_EXE_PATH)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SFML_BUILD_WINDOW ON CACHE BOOL "" FORCE)
set(SFML_BUILD_GRAPHICS ON CACHE BOOL "" FORCE)
set(SFML_BUILD_AUDIO OFF CACHE BOOL "" FORCE)
set(SFML_BUILD_NETWORK OFF CACHE BOOL "" FORCE)
set(SFML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SFML_BUILD_DOC OFF CACHE BOOL "" FORCE)
set(SFML_BUILD_TEST_SUITE OFF CACHE BOOL "" FORCE)
set(SFML_CONFIGURE_EXTRAS OFF CACHE BOOL "" FORCE)
set(SFML_INSTALL_PKGCONFIG_FILES OFF CACHE BOOL "" FORCE)
# Keep FreeType/HarfBuzz/SheenBidi inside the static SFML build instead of
# taking shared system copies. Native window/OpenGL libraries remain system
# dependencies where the OS requires them.
set(SFML_USE_SYSTEM_DEPS OFF CACHE BOOL "" FORCE)

# Prefer the GUI's own bundled SFML source so configuration never depends on a
# solver checkout. SFML may still FetchContent its pinned FreeType/HarfBuzz/
# SheenBidi sources when static bundled dependencies are selected.
set(CFD_BUNDLED_SFML_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sfml")
if(EXISTS "${CFD_BUNDLED_SFML_DIR}/CMakeLists.txt")
    add_subdirectory(
        "${CFD_BUNDLED_SFML_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/sfml-build"
    )
else()
    message(STATUS "Bundled SFML is absent; fetching pinned SFML 3.1.0.")
    FetchContent_Declare(
        SFML
        URL https://github.com/SFML/SFML/archive/refs/tags/3.1.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(SFML)
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

add_library(mask_ui_core STATIC
    src/ExplorerTarget.cpp
    src/FluidSolverRun.cpp
    src/GeometryProcessor.cpp
    src/NumericInput.cpp
    src/SectionAdapter.cpp
    src/tiny_obj_loader_impl.cpp
    src/VtkFrame.cpp
    src/VelocityOverlay.cpp
)

target_include_directories(mask_ui_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CFD_TINYOBJ_DIR}"
)

target_compile_features(mask_ui_core PUBLIC cxx_std_17)

if(CFD_UI_ENABLE_AVX2)
    if(MSVC)
        target_compile_options(mask_ui_core PRIVATE /arch:AVX2)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(mask_ui_core PRIVATE -mavx2)
    else()
        message(FATAL_ERROR
            "CFD_UI_ENABLE_AVX2 is ON, but this compiler has no configured AVX2 flag.")
    endif()
endif()

add_executable(cfd_mask_ui_optimized
    src/main.cpp
    src/Application.cpp
)

target_link_libraries(cfd_mask_ui_optimized
    PRIVATE
        mask_ui_core
        SFML::Graphics
)

target_compile_definitions(cfd_mask_ui_optimized
    PRIVATE
        CFD_SOLVER_EXE="${CFD_SOLVER_EXE_PATH}"
        CFD_MASK_UI_VERSION="${PROJECT_VERSION}"
        NOMINMAX
        WIN32_LEAN_AND_MEAN
)

target_compile_features(cfd_mask_ui_optimized PRIVATE cxx_std_17)
set_target_properties(cfd_mask_ui_optimized PROPERTIES OUTPUT_NAME "Fluid Solver UI")

if(CFD_UI_ENABLE_AVX2)
    if(MSVC)
        target_compile_options(cfd_mask_ui_optimized PRIVATE /arch:AVX2)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(cfd_mask_ui_optimized PRIVATE -mavx2)
    endif()
endif()

if(CFD_UI_STATIC_RUNTIME AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(cfd_mask_ui_optimized PRIVATE -static-libgcc -static-libstdc++)
endif()

if(WIN32)
    set_target_properties(cfd_mask_ui_optimized PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_libraries(
        cfd_mask_ui_optimized
        PRIVATE
            SFML::Main
            Comdlg32
            Shell32
            Comctl32
    )
endif()

if(EXISTS "${CFD_SOLVER_EXE}")
    add_custom_command(
        TARGET cfd_mask_ui_optimized
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CFD_SOLVER_EXE}"
            "$<TARGET_FILE_DIR:cfd_mask_ui_optimized>/Fluid Solver.exe"
        COMMENT "Copying the selected Fluid Solver beside the GUI"
        VERBATIM
    )
endif()

include(CTest)

if(BUILD_TESTING)
    function(add_mask_ui_test TARGET_NAME)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/${TARGET_NAME}.cpp")
            add_executable(
                ${TARGET_NAME}
                "tests/${TARGET_NAME}.cpp"
            )
            target_link_libraries(${TARGET_NAME} PRIVATE mask_ui_core)
            add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
        endif()
    endfunction()

    foreach(MASK_UI_TEST
        ExplorerTargetTests
        VtkFrameTests
        FluidSolverRunTests
        NumericInputTests
        VelocityOverlayTests
        SectionAdapterTests
        GeometryProcessorTests
    )
        add_mask_ui_test(${MASK_UI_TEST})
    endforeach()

    if(EXISTS "${CFD_SOLVER_EXE}")
        add_mask_ui_test(SolverCompatibilityTests)
        if(TARGET SolverCompatibilityTests)
            target_compile_definitions(SolverCompatibilityTests
                PRIVATE CFD_SOLVER_EXE="${CFD_SOLVER_EXE_PATH}"
            )
        endif()
    endif()
endif()
