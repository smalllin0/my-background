#include "my_background.h"
#include "sdkconfig.h"
#include <esp_log.h>


#define TAG "MyBackground"


#define TASK_ARRIVED_EVENT  BIT0    // 任务到达事件
#define INSTANCE_ALIVED     BIT1    // 实例存在事件
#define CONFIG_DISPLAY_TASK_HANDLE_COMPLETE false


MyBackground::MyBackground()
{

    // 创建后台管理任务（不再创建 event group，使用 task notify）
    auto result = xTaskCreatePinnedToCore(
        [](void* arg){
            auto* background = reinterpret_cast<MyBackground*>(arg);
            background->BackgroundHandler();
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
    }
}

MyBackground::~MyBackground()
{
    // 单例函数，不应该析构（除非是操作系统回收）
    if (background_) {
        xTaskNotifyGive(background_);
        vTaskDelete(background_);
    }
}


/// @brief 调度一个任务到后台运行
/// @param fn 任务函数
/// @param task_name 任务名
/// @param arg 任务函数参数
/// @param free_fn 任务清理时资源回收函数
/// @return 调度到后台队列成功返回true
bool MyBackground::Schedule(RawTask fn, const char* task_name, void* arg, FreeFun free_fn) 
{
    if (!fn) {
        ESP_LOGW(TAG, "Attempt to schedule null task function.");
        return false;
    }
    if (task_list_.full()) {
        printf("Task buffer full, task dropped.\n");
        return false;
    }
    // 注意：引用捕获在这里是安全的，因为：
    // 1. lambda 在 push_back 内部立即执行
    // 2. Task 构造函数拷贝所有数据
    // 3. 构造在函数返回前完成
    auto ok = task_list_.construct([=](Task* task){
        new (task) Task(task_name, fn, arg, free_fn);
    });
    if (ok) {
        // 计算更新最大任务数量
        auto count = task_list_.size();
        if ( count > max_tasks_count_ ) {
            max_tasks_count_ = count;
        }
        if (background_) xTaskNotifyGive(background_);
    }

    return ok;
}


/// @brief 调度一个可捕获任务到后台运行
/// @param fn 任务函数
/// @param task_name 任务名
/// @param arg 任务函数参数
/// @param free_fn 任务清理时资源回收函数
/// @return 调度到后台队列成功返回true
#if CONFIG_ENABLE_TASK_CAPTURE_SUPPORT
bool MyBackground::captureSchedule(TaskWithArgFun fn, const char* task_name, void* arg, FreeFun free_fn) 
{
    if (!fn) {
        ESP_LOGW(TAG, "Attempt to schedule null task function.");
        return false;
    }
    if (task_list_.full()) {
        printf("Task buffer full, task dropped.\n");
        return false;
    }
    // 注意：引用捕获在这里是安全的，因为：
    // 1. lambda 在 push_back 内部立即执行
    // 2. Task 构造函数拷贝所有数据
    // 3. 构造在函数返回前完成
    auto ok = task_list_.construct([&task_name, &fn, &arg, &free_fn](Task* task){
        new (task) Task(task_name, fn, arg, free_fn);
    });
    if (ok) {
        // 计算更新最大任务数量
        auto count = task_list_.size();
        if ( count > max_tasks_count_ ) {
            max_tasks_count_ = count;
        }
        if (background_) xTaskNotifyGive(background_);
    }

    return ok;
}
#endif

/// @brief 调度一个任务到后台运行(无参数任务函数兼容版本，后续可能删除)
#if CONFIG_SUPPORT_TASK_WITHOUT_ARG
bool MyBackground::Schedule(TaskWithoutArgFun fn, const std::string& task_name) 
{
    if (!fn) {
        ESP_LOGW(TAG, "Attempt to schedule null task function.");
        return false;
    }
    auto function = [&fn](void*) { fn(); };
    return captureSchedule(function, task_name.c_str(), nullptr, nullptr);
}
#endif

size_t MyBackground::Clear(const std::string& name)
{
    size_t count = 0;

    if (name.empty()) {
        count = task_list_.size();
        task_list_.clear([](Task& task){
            task.Cleanup();
        });
    } else {
        task_list_.remove_if([&](Task& task){
            if (name == task.name) {
                count ++;
                task.Cleanup();
                return true;
            }
            return false;
        });
    }
    return count;
}


void MyBackground::PrintBackgroundInfo()
{
    Schedule([](void* arg){
        auto self = reinterpret_cast<MyBackground*>(arg);
        if (self->max_tasks_count_ <= (CONFIG_MAX_BACKGROUND_TASKS >> 1)) {
            ESP_LOGI(TAG, "current tasks: %d, Max background tasks: %d", self->task_list_.size(), self->max_tasks_count_);
        } else if (self->max_tasks_count_ <= ((CONFIG_MAX_BACKGROUND_TASKS >> 1) + (CONFIG_MAX_BACKGROUND_TASKS >> 2))) {
            ESP_LOGW(TAG, "current tasks: %d, Max background tasks: %d", self->task_list_.size(), self->max_tasks_count_);
        } else {
            ESP_LOGE(TAG, "current tasks: %d, Max background tasks: %d", self->task_list_.size(), self->max_tasks_count_);
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
    }, "PrintBg", this);
}

/// @brief 后台管理任务
void MyBackground::BackgroundHandler()
{
    while(true) {

        // 等待通知
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (!task_list_.empty()) {
            task_list_.consume_front([](Task* task){
                task->execute();
#if CONFIG_DISPLAY_TASK_HANDLE_COMPLETE
                ESP_LOGI(TAG, "%s: 处理完成", task->name);
#endif
            });
        }
    }
    ESP_LOGE(TAG, "Background manager task run out of range!");
    background_ = nullptr;
    vTaskDelete(nullptr);
}

