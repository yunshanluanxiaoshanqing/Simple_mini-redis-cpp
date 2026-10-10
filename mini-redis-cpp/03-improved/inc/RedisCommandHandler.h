#pragma once

#include <string>
#include <vector>
#include <sstream>
#include <algorithm>

#include "RedisDatabase.h"

// 命令解析与分发：输入一条完整命令（RESP 或内联文本），输出 RESP 响应。
class RedisCommandHandler {
public:
    RedisCommandHandler() = default;
    ~RedisCommandHandler() = default;

    // 处理一条完整命令，返回 RESP 编码的响应（保证以 \r\n 结尾）
    std::string processCommand(const std::string& command);

    // 从接收缓冲区头部提取一条完整命令（处理粘包/半包）。
    // 返回  1: 提取出一条完整命令（写入 out）；
    //       0: 数据不完整，等待更多数据；
    //      -1: 协议错误（已消费错误部分，调用方应回复 -ERR Protocol error）。
    static int extractCommand(std::string& buffer, std::string& out);
};
