# AutoRemesher — Qt-free algorithm core + command line front end.
#
# The original build was a qmake project (autoremesher.pro) that linked the
# remeshing core against the Qt GUI.  This file builds the same core sources as
# a plain static library and drives them from a Qt-free CLI, so the algorithm
# can be compiled and measured on machines without Qt.
#
# The Qt GUI is *not* part of this build.  Every Qt source file, the Qt
# resources (.qrc, shaders/, resources/) and the original qmake project file
# have been moved to legacy/gui-qt/ and frozen there; see legacy/README.md and
# docs/GUI-LEGACY.md for the inventory, the freeze rationale and the (unverified)
# restore procedure.  src/ now holds only the shared core, the Qt-free CLI and
# tiny_obj_loader.h.
#
# Layout of the two products:
#   autoremesher_core  STATIC  src/AutoRemesher/*.cpp
#                              thirdparty/isotropicremesher/*.cpp
#                              thirdparty/meshoptimizer/src/{simplifier,indexgenerator}.cpp
#   autoremesher_cli   EXE     src/cli/main_cli.cpp
#
# TBB: the vendored thirdparty/tbb (2017) tree is used.  By default the import
# library produced by a separate CMake build of that tree is linked
# (build/tbb/tbb.lib + tbb.dll).  Set AUTOREMESHER_TBB_FROM_SOURCE=ON — or just
# leave the prebuilt library missing — to compile the same TBB sources into a
# static library inside this build instead.

cmake_minimum_required(VERSION 3.20)

project(AutoRemesher
    VERSION 1.2.0
    DESCRIPTION "Automatic quad remeshing tool"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# The Qt GUI has been moved out of the main build into legacy/gui-qt/ and is
# built there with qmake, never with CMake.  The option below stays only so that
# a build script still asking for a GUI target fails immediately and loudly
# instead of silently producing a CLI-only tree.
option(AUTOREMESHER_ENABLE_LEGACY_QT_GUI
    "Deprecated: the Qt GUI now lives in legacy/gui-qt and needs qmake, not CMake"
    OFF)
if(AUTOREMESHER_ENABLE_LEGACY_QT_GUI)
    message(FATAL_ERROR "Qt GUI 已移入 legacy/gui-qt，主链不再提供 GUI 目标")
endif()

set(AUTOREMESHER_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------------------------
# Experiment switches (M4).  Both default OFF; turn one on only to re-run an A/B.
# ---------------------------------------------------------------------------
# Both default OFF: the M4 measurement (docs/ARCHITECTURE-REFACTOR-PLAN.md, appendix A.2-1)
# showed neither candidate passes G1/G5/G7 together, so the shipped behaviour
# stays the historical one and the switches stay available for the follow-up
# work items M4-1b / M4-5b.
option(AUTOREMESHER_M4_PROGRESSIVE_ROUNDING
    "Fix integer variables progressively instead of all in the first solve" OFF)
option(AUTOREMESHER_M4_CREASE_AVERAGE
    "Average all crease constraints of a face in the frame field" OFF)

# ---------------------------------------------------------------------------
# TBB
# ---------------------------------------------------------------------------

option(AUTOREMESHER_TBB_FROM_SOURCE
    "Compile the vendored TBB sources into a static library instead of linking a prebuilt tbb.lib"
    OFF)

set(AUTOREMESHER_TBB_PREBUILT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/tbb"
    CACHE PATH "Directory holding the prebuilt vendored tbb.lib/tbb.dll")
set(AUTOREMESHER_TBB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tbb"
    CACHE PATH "Vendored TBB source tree")

set(AUTOREMESHER_TBB_PREBUILT_LIB "${AUTOREMESHER_TBB_PREBUILT_DIR}/tbb.lib")
set(AUTOREMESHER_TBB_PREBUILT_DLL "${AUTOREMESHER_TBB_PREBUILT_DIR}/tbb.dll")

set(AUTOREMESHER_TBB_DLL "" CACHE INTERNAL "")

if(NOT AUTOREMESHER_TBB_FROM_SOURCE AND EXISTS "${AUTOREMESHER_TBB_PREBUILT_LIB}")
    message(STATUS "AutoRemesher: linking prebuilt vendored TBB from ${AUTOREMESHER_TBB_PREBUILT_DIR}")
    add_library(autoremesher_tbb SHARED IMPORTED)
    set_target_properties(autoremesher_tbb PROPERTIES
        IMPORTED_IMPLIB "${AUTOREMESHER_TBB_PREBUILT_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${AUTOREMESHER_TBB_ROOT}/include")
    if(EXISTS "${AUTOREMESHER_TBB_PREBUILT_DLL}")
        set_target_properties(autoremesher_tbb PROPERTIES
            IMPORTED_LOCATION "${AUTOREMESHER_TBB_PREBUILT_DLL}")
        set(AUTOREMESHER_TBB_DLL "${AUTOREMESHER_TBB_PREBUILT_DLL}" CACHE INTERNAL "")
    endif()
    set(AUTOREMESHER_TBB_MODE "prebuilt-shared" CACHE INTERNAL "")
else()
    message(STATUS "AutoRemesher: compiling vendored TBB sources into a static library")
    enable_language(ASM_MASM)

    file(GLOB AUTOREMESHER_TBB_SOURCES
        "${AUTOREMESHER_TBB_ROOT}/src/tbb/*.cpp"
        "${AUTOREMESHER_TBB_ROOT}/src/old/*.cpp")
    # src/old holds pre-2011 compatibility shims plus their tests; drop the tests.
    file(GLOB AUTOREMESHER_TBB_OLD_TESTS "${AUTOREMESHER_TBB_ROOT}/src/old/test*.cpp")
    if(AUTOREMESHER_TBB_OLD_TESTS)
        list(REMOVE_ITEM AUTOREMESHER_TBB_SOURCES ${AUTOREMESHER_TBB_OLD_TESTS})
    endif()
    # The RML client is part of libtbb; rml_tbb.cpp registers the TBB side.
    list(APPEND AUTOREMESHER_TBB_SOURCES
        "${AUTOREMESHER_TBB_ROOT}/src/rml/client/rml_tbb.cpp")

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        file(GLOB AUTOREMESHER_TBB_ASM "${AUTOREMESHER_TBB_ROOT}/src/tbb/intel64-masm/*.asm")
        set(AUTOREMESHER_TBB_ASM_DEFS "/DEM64T=1")
    else()
        file(GLOB AUTOREMESHER_TBB_ASM "${AUTOREMESHER_TBB_ROOT}/src/tbb/ia32-masm/*.asm")
        set(AUTOREMESHER_TBB_ASM_DEFS "")
    endif()
    list(APPEND AUTOREMESHER_TBB_ASM ${AUTOREMESHER_TBB_ASM})

    add_library(autoremesher_tbb STATIC ${AUTOREMESHER_TBB_SOURCES})
    target_include_directories(autoremesher_tbb PUBLIC
        "${AUTOREMESHER_TBB_ROOT}/include"
        "${AUTOREMESHER_TBB_ROOT}/src"
        "${AUTOREMESHER_TBB_ROOT}/src/rml/include")
    target_compile_definitions(autoremesher_tbb PUBLIC
        __TBB_BUILD=1
        USE_WINTHREAD
        DO_ITT_NOTIFY
        _WIN32_WINNT=0x0600
        _CRT_SECURE_NO_DEPRECATE)
    if(MSVC)
        # TBB 2017 predates the stricter MSVC default; relax what it trips over.
        target_compile_options(autoremesher_tbb PRIVATE
            /GS- /wd4267 /wd4800 /wd4146 /wd4244 /wd4018 /wd4996 /bigobj)
        set_source_files_properties(${AUTOREMESHER_TBB_ASM} PROPERTIES
            LANGUAGE ASM_MASM
            COMPILE_FLAGS "${AUTOREMESHER_TBB_ASM_DEFS}")
    endif()
    set(AUTOREMESHER_TBB_MODE "bundled-static" CACHE INTERNAL "")
endif()

# ---------------------------------------------------------------------------
# autoremesher_core — the Qt-free remeshing pipeline
# ---------------------------------------------------------------------------

# The module source list is generated (tools/gen_sources.py) instead of globbed,
# so adding or moving a file is an explicit diff and a stale list fails
# tools/gen_sources.py --check in CI.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/core_sources.cmake)
set(AUTOREMESHER_CORE_SOURCES ${AUTOREMESHER_CORE_MODULE_SOURCES})

list(APPEND AUTOREMESHER_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/isotropicremesher/isotropicremesher.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/isotropicremesher/isotropichalfedgemesh.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/isotropicremesher/axisalignedboundingboxtree.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/meshoptimizer/src/simplifier.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/meshoptimizer/src/indexgenerator.cpp")

add_library(autoremesher_core STATIC ${AUTOREMESHER_CORE_SOURCES})

target_include_directories(autoremesher_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/eigen"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/isotropicremesher"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/meshoptimizer/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(autoremesher_core PUBLIC _USE_MATH_DEFINES NOMINMAX)
if(AUTOREMESHER_M4_PROGRESSIVE_ROUNDING)
    target_compile_definitions(autoremesher_core PRIVATE AUTOREMESHER_M4_PROGRESSIVE_ROUNDING=1)
endif()
if(AUTOREMESHER_M4_CREASE_AVERAGE)
    target_compile_definitions(autoremesher_core PRIVATE AUTOREMESHER_M4_CREASE_AVERAGE=1)
endif()
target_compile_definitions(autoremesher_core PRIVATE NDEBUG)

if(MSVC)
    # /bigobj: Eigen and the framefield solver produce very large translation
    # units.  /fp:fast matches the release flags of autoremesher.pro.
    target_compile_options(autoremesher_core PRIVATE /O2 /Ob2 /fp:fast /bigobj /GL- /wd4819)
endif()

target_link_libraries(autoremesher_core PUBLIC autoremesher_tbb)

# ---------------------------------------------------------------------------
# autoremesher_cli — headless front end, same parameters as src/main.cpp
# ---------------------------------------------------------------------------

add_executable(autoremesher_cli "${CMAKE_CURRENT_SOURCE_DIR}/src/cli/main_cli.cpp")

target_include_directories(autoremesher_cli PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")


target_link_libraries(autoremesher_cli PRIVATE autoremesher_core)

if(MSVC)
    # C4819: third-party headers are UTF-8 and the default console code page is not.
    target_compile_options(autoremesher_cli PRIVATE /O2 /Ob2 /bigobj /GL- /wd4819)
endif()

# The vendored TBB is a DLL; keep it next to the executable.
if(AUTOREMESHER_TBB_DLL)
    add_custom_command(TARGET autoremesher_cli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${AUTOREMESHER_TBB_DLL}"
            "$<TARGET_FILE_DIR:autoremesher_cli>")
endif()

# No Qt targets are defined anywhere in this file.  The only products are
# autoremesher_core (static library) and autoremesher_cli (executable); the GUI
# target list lives in legacy/gui-qt/autoremesher.pro and is not validated here.
