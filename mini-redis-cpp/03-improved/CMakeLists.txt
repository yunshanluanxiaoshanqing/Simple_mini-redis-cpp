cmake_minimum_required(VERSION 4.3.3)
project(redis_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

# 核心库：数据库 + 命令处理
add_library(redis_core STATIC
    src/RedisDatabase.cpp
    src/RedisCommandHandler.cpp
)
target_include_directories(redis_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_link_libraries(redis_core PUBLIC Threads::Threads)
target_compile_options(redis_core PRIVATE -Wall -Wextra)

# 服务器
add_executable(redis-server
    src/main.cpp
    src/RedisServer.cpp
)
target_link_libraries(redis-server PRIVATE redis_core)

# 客户端
add_executable(redis-cli client/main.cpp)
target_link_libraries(redis-cli PRIVATE redis_core)

if(WIN32)
    target_link_libraries(redis-server PRIVATE ws2_32)
    target_link_libraries(redis-cli PRIVATE ws2_32)
endif()

# MinGW（无论 win32 还是 posix 线程模型）一律静态链接运行时，
# 避免与 PATH 中其他发行版的 libstdc++/libwinpthread 混载导致崩溃
if(MINGW)
    foreach(tgt redis-server redis-cli)
        target_link_options(${tgt} PRIVATE -static)
    endforeach()
endif()

foreach(tgt redis-server redis-cli)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4)
    else()
        target_compile_options(${tgt} PRIVATE -Wall -Wextra)
    endif()
endforeach()
