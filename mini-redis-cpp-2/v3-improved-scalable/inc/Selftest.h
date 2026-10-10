#pragma once

// Selftest.h — 内置语义自检（不依赖网络与外部工具）
namespace redis {
/// 运行全部自检；返回 0 表示全部通过，非 0 为失败数量（最多 127）。
int runSelfTest();
}  // namespace redis
