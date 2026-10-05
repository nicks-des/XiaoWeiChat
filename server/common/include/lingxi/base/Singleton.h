/**
 * @file Singleton.h
 * @brief CRTP 单例模板：线程安全（C++11 魔法静态），继承即得单例语义。
 */
#pragma once

#include "lingxi/base/NonCopyable.h"

namespace lingxi {

/**
 * @brief 单例模板基类。
 * @tparam T 派生类型（CRTP），需可默认构造。
 *
 * 用法：class Foo : public Singleton<Foo> { friend class Singleton<Foo>; ... };
 */
template <typename T>
class Singleton : private NonCopyable {
public:
    /**
     * @brief 获取全局唯一实例（首次调用时构造，线程安全）。
     * @return T& 实例引用
     */
    static T& instance() {
        static T s_instance;
        return s_instance;
    }

protected:
    Singleton() = default;
    ~Singleton() = default;

private:
    friend T; ///< 允许派生类访问受保护的构造函数
};

} // namespace lingxi
