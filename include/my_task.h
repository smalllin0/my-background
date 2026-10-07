#ifndef MY_TASK_H_
#define MY_TASK_H_

#include "sdkconfig.h"


using RunFn  = void (*)(void* arg);
using FreeFn = void (*)(void* arg, bool executed);

/// @brief 后台任务基类
/// @note 派生类不得添加任何数据成员
///       所有任务参数通过 arg 访问，由 emplace<T>() 自动选择内联或堆存储
struct BgTask {
    static constexpr size_t STORAGE_SIZE = CONFIG_BG_INLINE_DATA_SIZE;

    using Destroyer = void (*)(void*, bool) noexcept;

    void*   arg{nullptr};                       // 用户参数指针
    bool    data_on_heap{false};                // 参数数据是否存放在堆上
    bool    executed{false};                    // 是否已经真正运行过
    char    name[CONFIG_BG_NAME_LEN]{};         // 任务名称（零初始化）
    Destroyer destroy{nullptr};                 // 用户释放参数的函数
    alignas(std::max_align_t) std::byte storage[STORAGE_SIZE];

    /// @brief 后台任务运行入口
    /// @note 先置位 executed 再 Run()，保证 destroy 能看到正确的执行状态
    void Invoke() {
        executed = true;
        Run();
    }

    virtual ~BgTask() {
        if (arg && destroy) {
            destroy(arg, data_on_heap);
            arg = nullptr;
        }
    }

    /// @brief 任务的参数对象构造函数
    template <typename T, typename... Args>
    T* emplace(Args&&... args) {
        if constexpr (sizeof(T) <= STORAGE_SIZE &&
                      alignof(T) <= alignof(std::max_align_t)) {
            arg = new (storage) T(std::forward<Args>(args)...);
            data_on_heap = false;
        } else {
            arg = new T(std::forward<Args>(args)...);
            data_on_heap = true;
        }
        destroy = [](void* ctx, bool on_heap) noexcept {
            auto* d = static_cast<T*>(ctx);
            if (on_heap) {
                delete d;
            } else {
                d->~T();
            }
        };

        return static_cast<T*>(arg);
    }

    /// @brief 任务参数安全转换函数（非 const 版）
    template <typename T>
    T* get() noexcept { return static_cast<T*>(arg); }

    /// @brief 任务参数安全转换函数（const 版）
    template <typename T>
    const T* get() const noexcept { return static_cast<const T*>(arg); }
protected:
    /// @brief 任务后台运行逻辑，通过 Invoke() 调用
    virtual void Run() = 0;
};

/// @brief 常用任务包装（携带上下文指针 + 可选清理回调）
struct TaskWrapper : BgTask {
    struct Data {
        void*   ctx{nullptr};       // 用户上下文
        RunFn   run_fn{nullptr};    // 运行函数
        FreeFn  free_fn{nullptr};   // 清理函数
    };

    TaskWrapper(void* ctx, RunFn run, FreeFn free = nullptr) {
        emplace<Data>(ctx, run, free);
    }

    ~TaskWrapper() override {
        auto* d = get<Data>();
        if (d && d->free_fn) d->free_fn(d->ctx, executed);
    }

protected:
    void Run() override {
        auto* d = get<Data>();
        if (d && d->run_fn) d->run_fn(d->ctx);
    }
};


#endif /* MY_TASK_H_ */