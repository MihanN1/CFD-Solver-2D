cmake_minimum_required(VERSION 3.28)

project(CFDMaskUIOptimized
    VERSION 0.1.0
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(
    CFD_ROOT_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../CFD-Solver-2D"
    CACHE PATH "Path to the GUI dependency bundle root"
)
get_filename_component(CFD_ROOT_DIR "${CFD_ROOT_DIR}" ABSOLUTE)

if(NOT EXISTS "${CFD_ROOT_DIR}/lib/sfml/CMakeLists.txt"
   OR NOT EXISTS "${CFD_ROOT_DIR}/include/tiny_obj_loader.h"
   OR NOT EXISTS
       "${CFD_ROOT_DIR}/lib/stl_reader/include/stl_reader/stl_reader.h")
    message(FATAL_ERROR
        "GUI dependency bundle not found at ${CFD_ROOT_DIR}. "
        "Set -DCFD_ROOT_DIR=<path> to the dependency root."
    )
endif()
set(
    CFD_SOLVER_EXE
    ""
    CACHE FILEPATH "Path to the separately built friend solver executable"
)
if(CFD_SOLVER_EXE STREQUAL "")
    foreach(CFD_SOLVER_CANDIDATE
        "${CFD_ROOT_DIR}_build/bin/Release/cfd_app.exe"
        "${CFD_ROOT_DIR}/build/bin/cfd_app.exe"
        "${CFD_ROOT_DIR}/build/bin/Release/cfd_app.exe"
        "${CFD_ROOT_DIR}/out/manual/cfd_app.exe"
    )
        if(EXISTS "${CFD_SOLVER_CANDIDATE}")
            set(CFD_SOLVER_EXE "${CFD_SOLVER_CANDIDATE}")
            break()
        endif()
    endforeach()
endif()
get_filename_component(CFD_SOLVER_EXE "${CFD_SOLVER_EXE}" ABSOLUTE)
if(NOT EXISTS "${CFD_SOLVER_EXE}")
    message(FATAL_ERROR
        "Friend solver executable not found at ${CFD_SOLVER_EXE}. "
        "Build the current friend source separately or set "
        "-DCFD_SOLVER_EXE=<absolute-path>."
    )
endif()
file(GLOB_RECURSE
    CFD_SOLVER_CODE_INPUTS
    CONFIGURE_DEPENDS
    "${CFD_ROOT_DIR}/include/*.h"
    "${CFD_ROOT_DIR}/include/*.hpp"
    "${CFD_ROOT_DIR}/include/*.cuh"
    "${CFD_ROOT_DIR}/lib/stl_reader/include/*.h"
    "${CFD_ROOT_DIR}/lib/stl_reader/include/*.hpp"
    "${CFD_ROOT_DIR}/src/*.cpp"
    "${CFD_ROOT_DIR}/src/*.cu"
    "${CFD_ROOT_DIR}/src/*.cuh"
)
set(CFD_SOLVER_BUILD_INPUTS
    "${CFD_ROOT_DIR}/CMakeLists.txt"
    ${CFD_SOLVER_CODE_INPUTS}
)
file(TIMESTAMP
    "${CFD_SOLVER_EXE}"
    CFD_SOLVER_EXE_TIMESTAMP
    "%s"
    UTC
)
set(CFD_STALE_SOLVER_INPUTS)
foreach(CFD_SOLVER_BUILD_INPUT IN LISTS CFD_SOLVER_BUILD_INPUTS)
    file(TIMESTAMP
        "${CFD_SOLVER_BUILD_INPUT}"
        CFD_SOLVER_INPUT_TIMESTAMP
        "%s"
        UTC
    )
    if(CFD_SOLVER_INPUT_TIMESTAMP GREATER CFD_SOLVER_EXE_TIMESTAMP)
        list(APPEND
            CFD_STALE_SOLVER_INPUTS
            "${CFD_SOLVER_BUILD_INPUT}"
        )
    endif()
endforeach()
if(CFD_STALE_SOLVER_INPUTS)
    list(JOIN CFD_STALE_SOLVER_INPUTS "\n  " CFD_STALE_SOLVER_INPUT_LIST)
    message(FATAL_ERROR
        "Friend solver executable is stale. These build inputs are newer "
        "than ${CFD_SOLVER_EXE}:\n  ${CFD_STALE_SOLVER_INPUT_LIST}\n"
        "Build the current friend source separately, then configure with "
        "-DCFD_SOLVER_EXE=<absolute-path>."
    )
endif()
file(TO_CMAKE_PATH "${CFD_SOLVER_EXE}" CFD_SOLVER_EXE_PATH)
file(TO_CMAKE_PATH
    "${CFD_ROOT_DIR}/lib/stl_reader/tests/data"
    CFD_TEST_DATA_DIR
)
set(CFD_CAR_MODEL "")
foreach(CFD_CAR_MODEL_CANDIDATE
    "${CFD_ROOT_DIR}/models/Car.obj"
    "${CFD_ROOT_DIR}/../CAR.obj"
)
    if(EXISTS "${CFD_CAR_MODEL_CANDIDATE}")
        file(TO_CMAKE_PATH
            "${CFD_CAR_MODEL_CANDIDATE}"
            CFD_CAR_MODEL
        )
        break()
    endif()
endforeach()
if(CFD_CAR_MODEL STREQUAL "")
    message(FATAL_ERROR
        "Car OBJ test fixture was not found beside or inside the solver root."
    )
endif()

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

add_subdirectory(
    "${CFD_ROOT_DIR}/lib/sfml"
    "${CMAKE_CURRENT_BINARY_DIR}/sfml-build"
)

if(MSVC)
    add_compile_options(/utf-8)
endif()

add_library(mask_ui_core STATIC
    src/ExplorerTarget.cpp
    src/FriendRun.cpp
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
        "${CFD_ROOT_DIR}/include"
        "${CFD_ROOT_DIR}/lib/stl_reader/include"
)

target_compile_features(mask_ui_core PUBLIC cxx_std_17)

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
        NOMINMAX
        WIN32_LEAN_AND_MEAN
)

target_compile_features(cfd_mask_ui_optimized PRIVATE cxx_std_17)

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

add_custom_command(
    TARGET cfd_mask_ui_optimized
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CFD_SOLVER_EXE}"
        "$<TARGET_FILE_DIR:cfd_mask_ui_optimized>/cfd_app.exe"
    COMMENT "Bundling the current friend solver beside the GUI"
    VERBATIM
)

include(CTest)

if(BUILD_TESTING)
    add_executable(ExplorerTargetTests
        tests/ExplorerTargetTests.cpp
    )
    target_link_libraries(ExplorerTargetTests PRIVATE mask_ui_core)
    add_test(NAME ExplorerTargetTests COMMAND ExplorerTargetTests)

    add_executable(GeometryProcessorTests
        tests/GeometryProcessorTests.cpp
    )
    target_link_libraries(GeometryProcessorTests PRIVATE mask_ui_core)
    target_compile_definitions(GeometryProcessorTests
        PRIVATE
            CFD_TEST_DATA_DIR="${CFD_TEST_DATA_DIR}"
            CFD_CAR_MODEL="${CFD_CAR_MODEL}"
    )
    add_test(NAME GeometryProcessorTests COMMAND GeometryProcessorTests)

    add_executable(VtkFrameTests
        tests/VtkFrameTests.cpp
    )
    target_link_libraries(VtkFrameTests PRIVATE mask_ui_core)
    add_test(NAME VtkFrameTests COMMAND VtkFrameTests)

    add_executable(FriendRunTests
        tests/FriendRunTests.cpp
    )
    target_link_libraries(FriendRunTests PRIVATE mask_ui_core)
    add_test(NAME FriendRunTests COMMAND FriendRunTests)

    add_executable(NumericInputTests
        tests/NumericInputTests.cpp
    )
    target_link_libraries(NumericInputTests PRIVATE mask_ui_core)
    add_test(NAME NumericInputTests COMMAND NumericInputTests)

    add_executable(VelocityOverlayTests
        tests/VelocityOverlayTests.cpp
    )
    target_link_libraries(VelocityOverlayTests PRIVATE mask_ui_core)
    add_test(NAME VelocityOverlayTests COMMAND VelocityOverlayTests)

    add_executable(SectionAdapterTests
        tests/SectionAdapterTests.cpp
    )
    target_link_libraries(SectionAdapterTests PRIVATE mask_ui_core)
    add_test(NAME SectionAdapterTests COMMAND SectionAdapterTests)

    add_executable(ExactNinetyIntegrationTests
        tests/ExactNinetyIntegrationTests.cpp
    )
    target_link_libraries(
        ExactNinetyIntegrationTests
        PRIVATE
            mask_ui_core
    )
    target_compile_definitions(ExactNinetyIntegrationTests
        PRIVATE
            CFD_CAR_MODEL="${CFD_CAR_MODEL}"
            CFD_SOLVER_EXE="${CFD_SOLVER_EXE_PATH}"
    )
    add_test(
        NAME ExactNinetyIntegrationTests
        COMMAND ExactNinetyIntegrationTests
    )
endif()
