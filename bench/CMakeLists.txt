cmake_minimum_required(VERSION 3.20)
project(arbench CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# TBB (vendored 2017 static build produced by build/build_tbb.bat)
set(TBB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/tbb")
set(TBB_LIB "${CMAKE_CURRENT_SOURCE_DIR}/../build/tbb/tbb_static.lib")

file(GLOB CORE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src/AutoRemesher/*.cpp")
file(GLOB ISOTROPIC_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/isotropicremesher/*.cpp")
file(GLOB MESHOPT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/meshoptimizer/src/*.cpp")

add_executable(arbench
    main.cpp
    ${CORE_SRC}
    ${ISOTROPIC_SRC}
    ${MESHOPT_SRC}
)

target_include_directories(arbench PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/eigen"
    "${TBB_ROOT}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/meshoptimizer/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/isotropicremesher"
)

target_compile_definitions(arbench PRIVATE NOMINMAX _USE_MATH_DEFINES __TBB_NO_IMPLICIT_LINKAGE=1)

if(MSVC)
    target_compile_options(arbench PRIVATE /O2 /fp:fast /bigobj /utf-8 /W1 /Zi)
    target_link_options(arbench PRIVATE /LTCG /DEBUG)
    target_compile_options(arbench PRIVATE /GL)
    set_property(TARGET arbench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

target_link_libraries(arbench PRIVATE "${TBB_LIB}" psapi dbghelp)
