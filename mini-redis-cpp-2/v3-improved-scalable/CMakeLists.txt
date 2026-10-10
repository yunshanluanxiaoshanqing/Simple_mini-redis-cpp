cmake_minimum_required(VERSION 3.16)
project(mini_redis_v3 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

# 核心库：协议、存储、快照、命令、连接、线程池
add_library(redis_core_v3 STATIC
    src/NetCompat.cpp
    src/Resp.cpp
    src/Glob.cpp
    src/Storage.cpp
    src/Snapshot.cpp
    src/StoreFile.cpp
    src/Commands.cpp
    src/Client.cpp
    src/ThreadPool.cpp
)

target_include_directories(redis_core_v3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_link_libraries(redis_core_v3 PUBLIC Threads::Threads)

# 服务器
add_executable(v3-server
    src/main.cpp
    src/Server.cpp
    src/Selftest.cpp
)
target_link_libraries(v3-server PRIVATE redis_core_v3)

# 客户端
add_executable(v3-cli client/main.cpp)
target_link_libraries(v3-cli PRIVATE redis_core_v3)

if(WIN32)
    target_link_libraries(v3-server PRIVATE ws2_32)
    target_link_libraries(v3-cli PRIVATE ws2_32)
endif()

# 严格警告：本版本要求 -Wall -Wextra 零警告
foreach(tgt redis_core_v3 v3-server v3-cli)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

# GCC 16 在优化构建下会对 libstdc++ 的 shared_ptr/thread 控制块报
# -Warray-bounds 误报（第三方头文件）。只关闭这一条诊断。
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
    foreach(tgt redis_core_v3 v3-server v3-cli)
        target_compile_options(${tgt} PRIVATE -Wno-array-bounds)
    endforeach()
endif()

# MinGW 静态链接运行时，避免 PATH 上其他 libstdc++/libwinpthread 混载
if(MINGW)
    foreach(tgt v3-server v3-cli)
        target_link_options(${tgt} PRIVATE -static)
    endforeach()
endif()

enable_testing()
add_test(NAME selftest COMMAND v3-server --selftest)
