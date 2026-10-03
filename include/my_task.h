#ifndef MY_TASK_H_
#define MY_TASK_H_


#define CONFIG_BG_NAME_LEN          10      // 任务名长度(N * 4 - 2)
#define CONFIG_BG_INLINE_DATA_SIZE  32      // 内联数据块大小


using RunFn  = void (*)(void* arg);
using FreeFn = void (*)(void* arg, bool executed);

/// @brief 后台任务基类
/// @note 派生类不得添加任何数据成员
///       所有任务参数通过 arg 访问，由 emplace<T>() 自动选择内联或堆存储
struct BgTask {
    static constexpr size_t STORAGE_SIZE = CONFIG_BG_INLINE_DATA_SIZE;

    void*   arg{nullptr};                       // 用户参数指针
    bool    data_on_heap{false};                // 参数数据是否存放在堆上
    bool    executed{false};                    // 是否已经真正运行过
    char    name[CONFIG_BG_NAME_LEN]{};         // 任务名称（零初始化）
    alignas(std::max_align_t) std::byte storage[STORAGE_SIZE];

    /// @brief 后台任务运行入口
    /// @note 先置位 executed 再 Run()，保证 free_fn 能看到正确的执行状态
    void Invoke() {
        executed = true;
        Run();
    }

    virtual ~BgTask() = default;

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
        return static_cast<T*>(arg);
    }

    /// @brief 任务参数安全转换函数（非 const 版）
    template <typename T>
    T* get() noexcept { return static_cast<T*>(arg); }

    /// @brief 任务参数安全转换函数（const 版）
    template <typename T>
    const T* get() const noexcept { return static_cast<const T*>(arg); }

    /// @brief 任务的参数对象清理
    template <typename T>
    void destroy() {
        if (!arg) return;
        auto* d = static_cast<T*>(arg);
        if (data_on_heap) {
            delete d;
        } else {
            d->~T();
        }
        arg = nullptr;
        data_on_heap = false;
    }

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
        destroy<Data>();
    }

protected:
    void Run() override {
        auto* d = get<Data>();
        if (d && d->run_fn) d->run_fn(d->ctx);
    }
};


#endif /* MY_TASK_H_ */