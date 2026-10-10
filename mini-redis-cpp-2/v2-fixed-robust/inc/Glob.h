#pragma once

// Glob.h — 键名模式匹配
//
// 01-original 的 KEYS 直接忽略模式参数；02/03 版本改用 std::regex 逐个 key
// 编译正则（每次请求都构造 regex，且 glob→regex 的转义是有漏的）。
// 本版本手写线性回溯匹配，无正则、无分配、语义与 Redis stringmatchlen 对齐。

#include <string>

namespace redis {

/// 判断 text 是否匹配 pattern。
/// 支持：`*`（任意长度）、`?`（单字符）、`[abc]`、`[a-z]`、`[^abc]`、`\x` 转义。
bool globMatch(const std::string& pattern, const std::string& text);

}  // namespace redis
