/**
 * @file NonCopyable.h
 * @brief 不可拷贝基类：继承后自动删除拷贝构造与拷贝赋值。
 */
#pragma once

namespace lingxi {

/**
 * @brief 不可拷贝对象基类（空基类优化，无虚函数开销）。
 */
class NonCopyable {
protected:
    /// 仅允许派生类构造与析构
    NonCopyable() = default;
    ~NonCopyable() = default;

    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
};

} // namespace lingxi
