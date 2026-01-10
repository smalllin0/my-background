#ifndef _MY_BACKGROUND_H_
#define _MY_BACKGROUND_H_

#include <atomic>
#include <mutex>
#include <functional>
#include <string>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "my_linklist.h"



#define CONFIG_BG_NAME_LEN     12


using FreeFun = void(*)(void* arg);
#if CONFIG_ENABLE_TASK_CAPTURE_SUPPORT
    using TaskFun= std::function<void(void*)>;
#else
    using TaskFun = void(*)(void* arg);
#endif
#if CONFIG_SUPPORT_TASK_WITHOUT_ARG
    using TaskWithoutArgFun = std::function<void(void)>;
#endif

/// @brief 后台任务管理类
class MyBackground {
private:
    struct Task {
        char            name[CONFIG_BG_NAME_LEN];
        void*           arg{nullptr};
        TaskFun         fn{nullptr};
        FreeFun         free_fn{nullptr};

        Task() { name[0] = '\0'; }
        Task(const char* task_name, TaskFun fun, void* fn_arg = nullptr, FreeFun free_fun = nullptr)
            : arg(fn_arg), fn(fun), free_fn(free_fun)
        {
            strncpy(name, task_name, CONFIG_BG_NAME_LEN - 1);
            name[CONFIG_BG_NAME_LEN - 1] = '\0';
        }  
        ~Task() {
            Cleanup();
        }

        void Cleanup() {
            if (free_fn && arg) {
                free_fn(arg);
                arg = nullptr;
            }
        }

        Task(const Task&) = delete;
        Task& operator=(const Task& other) = delete;

        // 移动构造函数
        Task(Task&& other) noexcept
            : arg(other.arg), fn(std::move(other.fn)), free_fn(std::move(other.free_fn))
        {
            strcpy(name, other.name);
            other.name[0] = '\0';
            other.fn = nullptr;
            other.arg = nullptr;
            other.free_fn= nullptr;
        }
        Task& operator=(Task&& other) noexcept {
            if (this != &other) {
                strcpy(name, other.name);
                fn = std::move(other.fn);
                arg = other.arg;
                free_fn = std::move(other.free_fn);             
                other.name[0] = '\0';
                other.fn = nullptr;
                other.arg = nullptr;
                other.free_fn= nullptr;
            }
            return *this;
        }

        inline void execute() const {
            if (fn) fn(arg);            
        }
    };
public:
    static MyBackground& GetInstance() {
        static MyBackground background;
        return background;
    }

    size_t Clear(const std::string& name);
    bool Schedule(TaskFun fn, const char* task_name="", void* arg=nullptr, FreeFun free_fn=nullptr);
#if CONFIG_SUPPORT_TASK_WITHOUT_ARG
    bool Schedule(TaskWithoutArgFun fn, const std::string& task_name="");
#endif
    size_t  GetBackgroundTasks() const { return task_list_.size(); };
    void PrintBackgroundInfo();

private:

    MyBackground();
    ~MyBackground();

    MyBackground(const MyBackground&) = delete;
    MyBackground& operator=(const MyBackground&) = delete;

    void BackgroundHandler();
    
    size_t max_tasks_count_{0};
    std::atomic<bool>   clear_flag_{false};
    TaskHandle_t        background_{nullptr};   // 后台任务句柄
    MyList<Task, CONFIG_MAX_BACKGROUND_TASKS>   task_list_;
};

#endif
