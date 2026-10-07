#ifndef _MY_BACKGROUND_H_
#define _MY_BACKGROUND_H_

#include <atomic>
#include <string>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "my_linklist.h"
#include "my_task.h"
#include "sdkconfig.h"

/// @brief 后台任务管理类
///
/// 【调度策略】
///   - 每来一个任务：若队列从空变非空（即第一个任务），启动单次定时器
///   - 队列达到 CONFIG_BATCH_SIZE：立即通知后台，停掉定时器
///   - 定时器到期（自第一个任务起 CONFIG_WAIT_MS）：通知后台
///   - 后台消费完队列后回睡；下个任务到达时重新开始计时
///
/// 【线程安全】
///   - task_list_ 内部自带锁
///   - max_tasks_count_ / timer_active_ 为 std::atomic
///   - Schedule 可从任意线程调用
class MyBackground {
public:
    static MyBackground& GetInstance() {
        static MyBackground background;
        return background;
    }

    /// @brief 调度任务（便捷接口）
    /// @param name 任务名
    /// @param fn   任务函数，签名 void(void* arg)
    /// @param free 可选清理回调，签名 void(void* arg, bool executed)
    /// @param arg  传给 fn 和 free 的用户上下文指针
    bool Schedule(const char* name, RunFn fn, FreeFn free = nullptr, void* arg = nullptr) {
        return Schedule<TaskWrapper>(name, arg, fn, free);
    }

    /// @brief 通用调度接口
    /// @tparam T 任务类型，必须继承 BgTask
    template <typename T, typename... Args>
    bool Schedule(const char* name, Args&&... args) {
        static_assert(std::is_base_of_v<BgTask, T>, "任务必须继承自 BgTask");
        static_assert(sizeof(T) == sizeof(BgTask),
                      "任务不能添加成员变量，需要时只能放入 Data 结构中并使用 emplace<Data> 打包");
        static_assert(alignof(T) <= alignof(BgTask), "继承任务 alignment too strict");

        if (!background_) return false;
        auto ok = task_list_.construct([&](BgTask* slot) noexcept {
            new (slot) T(std::forward<Args>(args)...);
            SetTaskName(slot, name);
        });
        if (!ok) return false;

        NotifyTaskAdded();
        return true;
    }

    /// @brief 清理任务
    /// @param name 任务名；空字符串表示清空所有
    /// @return 被清理的任务数量
    /// @note 只清理"尚未开始执行"的任务。
    size_t Clear(const std::string& name);

    /// @brief 获取当前待处理任务数
    size_t GetBackgroundTasks() const { return task_list_.used_size(); }

    /// @brief 打印后台信息（调试用）
    void PrintBackgroundInfo();

private:
    MyBackground();
    ~MyBackground() = default;

    MyBackground(const MyBackground&) = delete;
    MyBackground& operator=(const MyBackground&) = delete;

    static void TimerCallback(TimerHandle_t timer);

    void SetTaskName(BgTask* slot, const char* name);
    void NotifyTaskAdded();
    void BackgroundHandler();

    std::atomic<bool>                           timer_active_{false};   // 定时器是否在计时
    std::atomic<size_t>                         max_tasks_count_{0};    // 历史最大任务数
    TaskHandle_t                                background_{nullptr};   // 后台任务句柄
    TimerHandle_t                               timer_{nullptr};        // 单次定时器句柄
    MyList<BgTask, CONFIG_MAX_BACKGROUND_TASKS> task_list_;             // 后台任务列表
};

#endif