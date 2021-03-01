cmake_minimum_required(VERSION 2.8)

project(httpng C CXX)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})
set(CMAKE_INCLUDE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake"
    ${CMAKE_INCLUDE_PATH})

# Find Tarantool and Lua dependecies
set(TARANTOOL_FIND_REQUIRED ON)
find_package(Tarantool REQUIRED)
include_directories(${TARANTOOL_INCLUDE_DIRS} third_party/h2o/include
    third_party include)

function(build_module module files)
    add_library(${module} SHARED ${files})
    set_target_properties(${module} PROPERTIES PREFIX "")
    #    add_dependencies(${module} api)
    if(TARGET_OS_DARWIN)
        set_target_properties(${module} PROPERTIES LINK_FLAGS
            "-undefined dynamic_lookup")
    endif(TARGET_OS_DARWIN)
endfunction()

add_subdirectory(third_party/wslay)

set(USE_STATIC_WSLAY ON)

add_subdirectory(third_party/h2o)
add_subdirectory(third_party/xtm)
add_subdirectory(third_party/msgpuck)

#set(HTTPNG_USE_LIBUV ON)

include_directories(${LUAJIT_INCLUDE_DIRS})
set("CMAKE_C_FLAGS" "${CMAKE_C_FLAGS} -Wall -Werror")

if (HTTPNG_USE_LIBUV)
    add_definitions(-DUSE_LIBUV)
endif (HTTPNG_USE_LIBUV)

set(HEADERS include/tarantool-httpng/httpng.h)

build_module(httpng httpng/httpng.c ${HEADERS})

if (HTTPNG_USE_LIBUV)
    target_link_libraries(httpng wslay libh2o)
else (HTTPNG_USE_LIBUV)
    target_link_libraries(httpng wslay libh2o-evloop)
endif (HTTPNG_USE_LIBUV)

target_link_libraries(httpng xtm_api_static)

build_module(sample_site examples/sample_site.c
	${HEADERS})
target_link_libraries(sample_site httpng msgpuck)

