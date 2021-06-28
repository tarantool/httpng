cmake_minimum_required(VERSION 2.8)

project(httpng C CXX)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})
set(CMAKE_INCLUDE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake"
    ${CMAKE_INCLUDE_PATH})

include(CheckCSourceCompiles)

macro(add_compile_flags langs)
    foreach(_lang ${langs})
        string (REPLACE ";" " " _flags "${ARGN}")
        set ("CMAKE_${_lang}_FLAGS" "${CMAKE_${_lang}_FLAGS} ${_flags}")
        unset (${_lang})
        unset (${_flags})
    endforeach()
endmacro(add_compile_flags)

option(ENABLE_ASAN "Enable AddressSanitizer, a fast memory error detector based on compiler instrumentation" OFF)
if (ENABLE_ASAN)
    if (CMAKE_COMPILER_IS_GNUCC)
        message(FATAL_ERROR
            "\n"
            " Tarantool does not support GCC's AddressSanitizer. Use clang:\n"
            " $ git clean -xfd; git submodule foreach --recursive git clean -xfd\n"
            " $ CC=clang CXX=clang++ cmake . -DENABLE_ASAN=ON && make -j\n"
            "\n")
    endif()

    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address -fsanitize-blacklist=${CMAKE_SOURCE_DIR}/asan/asan.supp")
    check_c_source_compiles("int main(void) {
        #include <sanitizer/asan_interface.h>
        void *x;
	    __sanitizer_finish_switch_fiber(x);
        return 0;
        }" ASAN_INTERFACE_OLD)
    check_c_source_compiles("int main(void) {
        #include <sanitizer/asan_interface.h>
        void *x;
	    __sanitizer_finish_switch_fiber(x, 0, 0);
        return 0;
    }" ASAN_INTERFACE_NEW)
    set(CMAKE_REQUIRED_FLAGS "")

    if (ASAN_INTERFACE_OLD)
        add_definitions(-DASAN_INTERFACE_OLD=1)
    elseif (ASAN_INTERFACE_NEW)
        add_definitions(-UASAN_INTERFACE_OLD)
    else()
        message(FATAL_ERROR "Cannot enable AddressSanitizer")
    endif()

    add_compile_flags("C;CXX" -fsanitize=address -fsanitize-blacklist=${CMAKE_SOURCE_DIR}/asan/asan.supp)
endif()

# Find Tarantool and Lua dependecies
set(TARANTOOL_FIND_REQUIRED ON)
find_package(Tarantool REQUIRED)
include_directories(${TARANTOOL_INCLUDE_DIRS} third_party/h2o/include
    third_party include)

function(build_module module files)
    add_library(${module} SHARED ${files})
    set_target_properties(${module} PROPERTIES PREFIX "")
    #    add_dependencies(${module} api)
    if(${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
        set_target_properties(${module} PROPERTIES LINK_FLAGS
            "-undefined dynamic_lookup")
    endif()
endfunction()

add_subdirectory(third_party/wslay EXCLUDE_FROM_ALL)
set(WSLAY_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wslay/lib/includes"
                       "${CMAKE_CURRENT_BINARY_DIR}/third_party/wslay/lib/includes")

set(USE_STATIC_WSLAY ON)

add_subdirectory(third_party/h2o EXCLUDE_FROM_ALL)
add_subdirectory(third_party/xtm EXCLUDE_FROM_ALL)

#set(HTTPNG_USE_LIBUV ON)

include_directories(${LUAJIT_INCLUDE_DIRS})
set("CMAKE_C_FLAGS" "${CMAKE_C_FLAGS} -Wall -Werror")

if (HTTPNG_USE_LIBUV)
    add_definitions(-DUSE_LIBUV)
endif (HTTPNG_USE_LIBUV)


#set(HEADERS include/tarantool-httpng/httpng.h)
set(HEADERS ${HEADERS} httpng/openssl_utils.h)

#set_source_files_properties(httpng/httpng.c PROPERTIES LANGUAGE CXX)
set (HTTPNG_C_SRC
                  httpng/httpng.c
                  httpng/openssl_utils.c
)

set(HTTPNG_SRC ${HTTPNG_C_SRC} ${HEADERS})

build_module(httpng_c "${HTTPNG_SRC}")

set(LIBS wslay xtm_api_static)

if (HTTPNG_USE_LIBUV)
    set(LIBS ${LIBS} libh2o)
else (HTTPNG_USE_LIBUV)
    set(LIBS ${LIBS} libh2o-evloop)
endif (HTTPNG_USE_LIBUV)

if (ENABLE_FD_LEAK_DETECTOR)
    set(LEAK httpng/fd-leak-detector.h)
    target_precompile_headers(httpng_c PUBLIC ${LEAK})
    target_precompile_headers(libh2o PUBLIC ${LEAK})
    target_precompile_headers(libh2o-evloop PUBLIC ${LEAK})
    target_precompile_headers(xtm_api_static PUBLIC ${LEAK})
    add_library(fd-leak-detector httpng/fd-leak-detector.cpp)
    set_property(TARGET fd-leak-detector PROPERTY POSITION_INDEPENDENT_CODE ON)
    set(LIBS ${LIBS} fd-leak-detector)
endif (ENABLE_FD_LEAK_DETECTOR)

target_link_libraries(httpng_c ${LIBS})
# FIXME: Make runtime configurable from Lua.
if (DISABLE_HTTP2)
    target_compile_definitions(httpng_c PRIVATE "DISABLE_HTTP2")
endif(DISABLE_HTTP2)

if (NOT(ENABLE_ASAN))
    # Just test
    add_custom_target(test-linkability ALL COMMAND "$ENV{SHELL}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/linkability-helper.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}/httpng.lua" "${CMAKE_CURRENT_SOURCE_DIR}/tests/linkability.lua")
    add_dependencies(test-linkability httpng_c)
endif (NOT(ENABLE_ASAN))

# Install library
install(TARGETS httpng_c LIBRARY DESTINATION ${TARANTOOL_INSTALL_LIBDIR})
install(FILES httpng.lua DESTINATION ${TARANTOOL_INSTALL_LUADIR})

add_executable(process_helper httpng/process_helper.c)
set_target_properties(process_helper PROPERTIES OUTPUT_NAME ${CMAKE_CURRENT_SOURCE_DIR}/tests/process_helper)
