#include "Glob.h"

#include <cstddef>

namespace redis {

namespace {

/// 匹配一个字符类 [...]。i 指向 '['，返回后指向 ']' 之后。
/// 语法：`*`/`?` 在类内按字面量处理，`a-z` 为区间，`[^...]`/`[!...]` 取反，
/// `[...]` 中的 `\` 转义下一个字符。未闭合的 '[' 按“不匹配”处理。
bool matchClass(const std::string& pattern, size_t& i, char c) {
    const size_t n = pattern.size();
    bool negate = false;
    ++i;  // 跳过 '['
    if (i < n && (pattern[i] == '^' || pattern[i] == '!')) {
        negate = true;
        ++i;
    }
    bool matched = false;
    bool closed = false;
    while (i < n) {
        if (pattern[i] == ']') {
            closed = true;
            ++i;
            break;
        }
        if (pattern[i] == '\\' && i + 1 < n) {
            if (c == pattern[i + 1]) matched = true;
            i += 2;
            continue;
        }
        if (i + 2 < n && pattern[i + 1] == '-' && pattern[i + 2] != ']') {
            if (c >= pattern[i] && c <= pattern[i + 2]) matched = true;
            i += 3;
            continue;
        }
        if (c == pattern[i]) matched = true;
        ++i;
    }
    if (!closed) return false;
    return negate ? !matched : matched;
}

}  // namespace

// 采用「最近星号 + 单层回退」的贪心实现（与 Redis stringmatchlen 的语义一致，
// 但不需要递归，因此不会有指数级回溯导致的栈/CPU 风险）。
bool globMatch(const std::string& pattern, const std::string& text) {
    const size_t pn = pattern.size();
    const size_t tn = text.size();
    size_t p = 0;
    size_t t = 0;
    size_t starP = std::string::npos;  // 最近一次 '*' 的模式位置
    size_t starT = 0;                  // 该 '*' 当时匹配到的文本位置

    while (t < tn) {
        if (p < pn && pattern[p] == '*') {
            starP = p++;
            starT = t;  // 先让 '*' 匹配空串；后续不匹配再回退扩展
            continue;
        }
        bool matched = false;
        if (p < pn) {
            const char pc = pattern[p];
            if (pc == '?') {
                matched = true;
                ++p;
            } else if (pc == '[') {
                matched = matchClass(pattern, p, text[t]);
            } else if (pc == '\\') {
                if (p + 1 < pn) {
                    matched = (pattern[p + 1] == text[t]);
                    p += 2;
                } else {
                    matched = false;  // 结尾孤立的反斜杠：按不匹配处理
                    ++p;
                }
            } else {
                matched = (pc == text[t]);
                ++p;
            }
        }
        if (matched) {
            ++t;
            continue;
        }
        // 不匹配且之前出现过 '*'：回退到该 '*' 之后，并让它多吃一个字符
        if (starP != std::string::npos) {
            p = starP + 1;
            t = ++starT;
            continue;
        }
        return false;
    }

    // 文本已耗尽：剩余模式必须全是 '*'
    while (p < pn && pattern[p] == '*') ++p;
    return p == pn;
}

}  // namespace redis
