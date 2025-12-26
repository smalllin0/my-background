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


using RawTask = void(*)(void* arg);
using FreeFun = void(*)(void* arg);
#if CONFIG_ENABLE_TASK_CAPTURE_SUPPORT
using TaskWithArgFun = std::function<void(void*)>;
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
        RawTask         raw_fn{nullptr};
        FreeFun         free_fn{nullptr};
#if CONFIG_ENABLE_TASK_CAPTURE_SUPPORT
        TaskWithArgFun  fn{nullptr};
#endif

        Task() { name[0] = '\0'; }
        Task(const char* task_name, RawTask fun, void* fn_arg = nullptr, FreeFun free_fun = nullptr)
            : arg(fn_arg), raw_fn(fun), free_fn(free_fun)
        {
            strncpy(name, task_name, CONFIG_BG_NAME_LEN - 1);
            name[CONFIG_BG_NAME_LEN - 1] = '\0';
        }
#if CONFIG_ENABLE_TASK_CAPTURE_SUPPORT
        Task(const char* task_name, TaskWithArgFun fun, void* fn_arg = nullptr, FreeFun free_fun = nullptr)
            : arg(fn_arg), free_fn(free_fun), fn(std::move(fun))
        {
            strncpy(name, task_name, CONFIG_BG_NAME_LEN - 1);
            name[CONFIG_BG_NAME_LEN - 1] = '\0';
        }
#endif
        
#if CONFIG_SUPPORT_TASK_WITHOUT_ARG        
        Task(const char* task_name, TaskWithoutArgFun fun, void* fn_arg = nullptr, FreeFun free_fn = nullptr)
        {
            strncpy(name, task_name, CONFIG_BG_NAME_LEN - 1);
            name[CONFIG_BG_NAME_LEN - 1] = '\0';

            fn = [func = std::move(fun)](void*) { func(); };
        }
#endif   
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
            : arg(other.arg), raw_fn(other.raw_fn), free_fn(other.free_fn)
        {
#if CONFIG_ENABLE_TASK_CAPTURE_SUPPORT
            fn = std::move(std::move(other.fn));
            other.fn = nullptr;
#endif
            strcpy(name, other.name);
            other.name[0] = '\0';
            other.raw_fn = nullptr;
            other.arg = nullptr;
            other.free_fn= nullptr;
        }
        Task& operator=(Task&& other) noexcept {
            if (this != &other) {
                Cleanup();

                strcpy(name, other.name);
                raw_fn = std::move(other.raw_fn);
                arg = other.arg;
                free_fn = std::move(other.free_fn);
#if CONFIG_ENABLE_TASK_CAPTURE_SUPPORT
                fn = std::move(std::move(other.fn));
                other.fn = nullptr;
#endif                
                other.name[0] = '\0';
                other.raw_fn = nullptr;
                other.arg = nullptr;
                other.free_fn= nullptr;
            }
            return *this;
        }

        inline void execute() const {
#if CONFIG_ENABLE_TASK_CAPTURE_SUPPORT
            if (raw_fn) {
                raw_fn(arg);
            } else if(fn) {
                fn(arg);
            }
#else
            if (raw_fn) raw_fn(arg);
#endif
             
        }
    };
public:
    static MyBackground& GetInstance() {
        static MyBackground background;
        return background;
    }

    size_t Clear(const std::string& name);
    bool Schedule(RawTask fn, const char* task_name=nullptr, void* arg=nullptr, FreeFun free_fn=nullptr);
#if CONFIG_ENABLE_TASK_CAPTURE_SUPPORT
    bool captureSchedule(TaskWithArgFun fn, const char* task_name="", void* arg=nullptr, FreeFun free_fn=nullptr);
#endif
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
