cmake_minimum_required(VERSION 3.16)
project(mini_redis_v1 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

# 核心库：协议、存储、命令、连接
add_library(redis_core_v1 STATIC
    src/NetCompat.cpp
    src/Resp.cpp
    src/Glob.cpp
    src/Storage.cpp
    src/Commands.cpp
    src/Client.cpp
)

target_include_directories(redis_core_v1 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_link_libraries(redis_core_v1 PUBLIC Threads::Threads)

# 服务器
add_executable(v1-server
    src/main.cpp
    src/Server.cpp
    src/Selftest.cpp
)
target_link_libraries(v1-server PRIVATE redis_core_v1)

# 客户端
add_executable(v1-cli client/main.cpp)
target_link_libraries(v1-cli PRIVATE redis_core_v1)

if(WIN32)
    target_link_libraries(v1-server PRIVATE ws2_32)
    target_link_libraries(v1-cli PRIVATE ws2_32)
endif()

# 严格警告：本版本要求 -Wall -Wextra 零警告
# （01/02 的 CMakeLists 完全没有开警告，低级错误因此长期潜伏；03 才补上）
foreach(tgt redis_core_v1 v1-server v1-cli)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

# GCC 16 在优化构建下会对 libstdc++ 的 shared_ptr/std::thread 控制块报
# -Warray-bounds 误报（第三方头文件，与本项目代码无关）。只关闭这一条诊断。
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
    foreach(tgt redis_core_v1 v1-server v1-cli)
        target_compile_options(${tgt} PRIVATE -Wno-array-bounds)
    endforeach()
endif()

# MinGW 静态链接运行时，避免 PATH 上其他 libstdc++/libwinpthread 混载
if(MINGW)
    foreach(tgt v1-server v1-cli)
        target_link_options(${tgt} PRIVATE -static)
    endforeach()
endif()

enable_testing()
add_test(NAME selftest COMMAND v1-server --selftest)
