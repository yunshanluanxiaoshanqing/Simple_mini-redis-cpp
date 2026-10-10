cmake_minimum_required(VERSION 3.16)
project(mini_redis_original CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

# 源码 include 形如 "inc/xxx.h"，头文件搜索路径为项目根目录
add_library(redis_core STATIC
    src/RedisDatabase.cpp
    src/RedisCommandHandler.cpp
)
target_include_directories(redis_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(redis_core PUBLIC Threads::Threads)

add_executable(redis-server src/main.cpp src/RedisServer.cpp)
target_link_libraries(redis-server PRIVATE redis_core)
