#include "my_background.h"
#include "sdkconfig.h"
#include <esp_log.h>

#define TAG "MyBackground"

#define CONFIG_DISPLAY_TASK_HANDLE_COMPLETE false

// ============================================================
// 构造 / 析构
// ============================================================

MyBackground::MyBackground()
{
    // 创建后台管理任务（使用 task notify 触发）
    auto result = xTaskCreatePinnedToCore(
        [](void* arg) {
            static_cast<MyBackground*>(arg)->BackgroundHandler();
        },
        "Bg_Task",
        CONFIG_STACK_SIZE,
        this,
        CONFIG_BACKGROUND_TASKS_PRIORITY,
        &background_,
        CONFIG_CORE_ID == -1 ? tskNO_AFFINITY : CONFIG_CORE_ID
    );
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create background manager task.");
        background_ = nullptr;
        return;   // 后台没起来，不创建定时器
    }

    // 单次定时器：仅当有任务等待且未凑够一批时启动
    timer_ = xTimerCreate(
        "Bg_Batch",
        pdMS_TO_TICKS(CONFIG_WAIT_MS),
        pdFALSE,                                // 单次
        this,
        &MyBackground::TimerCallback
    );
    if (!timer_) {
        ESP_LOGW(TAG, "Failed to create batch timer, "
                      "fall back to per-item notify.");
    }
}

// ============================================================
// 定时器回调
// ============================================================

void MyBackground::TimerCallback(TimerHandle_t timer)
{
    auto* self = static_cast<MyBackground*>(pvTimerGetTimerID(timer));

    // 定时器已到期，清零状态；下一次入队会重新启动它
    self->timer_active_.store(false, std::memory_order_relaxed);

    // 有任务才唤醒后台，避免空转
    if (self->background_ && self->task_list_.used_size() > 0) {
        xTaskNotifyGive(self->background_);
    }
}

// ============================================================
// 清理
// ============================================================

size_t MyBackground::Clear(const std::string& name)
{
    // 清空所有任务
    if (name.empty()) {
        size_t count = static_cast<size_t>(task_list_.used_size());
        task_list_.clear();
        return count;
    }

    // 按名字清空指定任务
    return static_cast<size_t>(task_list_.erase_if([&](BgTask& task) noexcept {
        return name == task.name;
    }));
}

// ============================================================
// 调试
// ============================================================

void MyBackground::PrintBackgroundInfo()
{
    Schedule(
        "PrintBg",
        [](void*) {
            auto& bg = MyBackground::GetInstance();
            const auto current = bg.GetBackgroundTasks();
            const auto max_cnt = bg.max_tasks_count_.load(std::memory_order_relaxed);

            if (max_cnt <= (CONFIG_MAX_BACKGROUND_TASKS >> 1)) {
                ESP_LOGI(TAG, "current tasks: %d, Max background tasks: %d",
                         (int)current, (int)max_cnt);
            } else if (max_cnt <= ((CONFIG_MAX_BACKGROUND_TASKS >> 1) +
                                   (CONFIG_MAX_BACKGROUND_TASKS >> 2))) {
                ESP_LOGW(TAG, "current tasks: %d, Max background tasks: %d",
                         (int)current, (int)max_cnt);
            } else {
                ESP_LOGE(TAG, "current tasks: %d, Max background tasks: %d",
                         (int)current, (int)max_cnt);
            }

        #ifdef CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS
            char task_list_buffer[1024];
            vTaskList(task_list_buffer);

            printf("Name        State     Pri      Stack  Num\n");
            printf("-----------------------------------------\n");
            printf("%s\n", task_list_buffer);
            printf("help: X(Running) B(Blocked) R(Ready) D(Deleted) S(Suspended)\n");
            printf("  Pri:    Priority, higher value indicates higher priority.\n");
            printf("  Stack:  Mini remaining stack space during task execution (in words).\n");
            printf("  Num:    Task creation sequence number.\n");
        #endif
        }
    );
}

// ============================================================
// 内部辅助
// ============================================================

void MyBackground::SetTaskName(BgTask* slot, const char* name)
{
    if (!name) {
        slot->name[0] = '\0';
        return;
    }

    strncpy(slot->name, name, CONFIG_BG_NAME_LEN - 1);
    slot->name[CONFIG_BG_NAME_LEN - 1] = '\0';
}

void MyBackground::NotifyTaskAdded()
{
    // 更新历史最大任务数
    size_t cur  = static_cast<size_t>(task_list_.used_size());
    size_t prev = max_tasks_count_.load(std::memory_order_relaxed);
    while (cur > prev &&
           !max_tasks_count_.compare_exchange_weak(prev, cur,
                                                   std::memory_order_relaxed)) {
    }

    // 定时器创建失败：退化为每次入队立即通知
    if (!timer_) {
        if (background_) xTaskNotifyGive(background_);
        return;
    }

    // 攒够一批：立即通知 + 停掉定时器
    if (cur >= CONFIG_BATCH_SIZE) {
        if (timer_active_.exchange(false, std::memory_order_relaxed)) {
            xTimerStop(timer_, 0);
        }
        if (background_) xTaskNotifyGive(background_);
        return;
    }

    // 不足一批：只在"第一个任务到达"时启动定时器
    // 后续任务到达时 CAS 会失败，不会重置计时
    bool expected = false;
    if (timer_active_.compare_exchange_strong(expected, true,
                                              std::memory_order_relaxed)) {
        xTimerStart(timer_, 0);
    }
}

// ============================================================
// 后台管理任务
// ============================================================

void MyBackground::BackgroundHandler()
{
    for (;;) {
        // 等待通知
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // 逐节点消费：
        //   - 每个节点从 used 链弹出、执行、放回 free 链
        //   - used 链在消费期间始终可见，Clear 的 erase_if 能看到剩余任务
        //   - 只有"正在执行的那一个"在窗口期对 Clear 不可见，
        //     但它 executed 已为 true，不属于漏删
        while (task_list_.consume_front([](BgTask* task) noexcept {
            task->Invoke();
#if CONFIG_DISPLAY_TASK_HANDLE_COMPLETE
            ESP_LOGI(TAG, "%s: 处理完成", task->name);
#endif
        })) {
            // 空循环体
        }
    }

    // 正常流程永远不会到这里
    ESP_LOGE(TAG, "Background manager task run out of range!");
    background_ = nullptr;
    vTaskDelete(nullptr);
}