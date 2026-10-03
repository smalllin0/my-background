# my-background

ESP32 上的轻量级后台任务调度组件。基于 `MyList` 固定容量对象池，支持零堆分配的
任务调度、批量唤醒、按名清理，适合从 lwIP 回调等中断/高优先级上下文快速提交任务，
在单独的低优先级后台任务中串行执行。

## 特性

- **零动态内存**：所有任务槽位编译期固定，`Schedule` 不触发 `new`
- **锁外构造 / 析构**：`Schedule` 提交、任务执行、清理都在锁外完成
- **批量唤醒**：攒够 `CONFIG_BATCH_SIZE` 或等满 `CONFIG_WAIT_MS` 才唤醒后台
- **可清理**：`Clear(name)` 支持按任务名精确删除待处理任务
- **线程安全**：`Schedule` 可从任意线程调用（包括 lwIP 回调）

## 文件结构

```
my-background/
├── include/
│   ├── my_background.h   # MyBackground 类
│   └── my_task.h         # BgTask / TaskWrapper
└── my_background.cc
```

依赖：`my-linklist`（`MyList<T, N>`）。

## 快速上手

### 1. 提交一个无参任务

```cpp
#include "my_background.h"

// 在任意线程调用
MyBackground::GetInstance().Schedule(
    "LogHello",                    // 任务名（≤ CONFIG_BG_NAME_LEN-1 字节）
    [](void*) {                    // run fn，无捕获 lambda 或函数指针
        ESP_LOGI("App", "hello from background");
    }
);
```

### 2. 携带上下文和清理回调

```cpp
struct MyCtx {
    int fd;
};

void OnDone(void* arg, bool executed) {
    auto* ctx = static_cast<MyCtx*>(arg);
    // executed == true  → run 已执行
    // executed == false → 任务被 Clear 或调度失败，run 未执行
    delete ctx;
}

MyBackground::GetInstance().Schedule(
    "HandleConn",
    [](void* arg) {
        auto* ctx = static_cast<MyCtx*>(arg);
        // 执行耗时逻辑（网络 IO / 解析 / 状态机更新）
    },
    OnDone,                         // 可选清理
    new MyCtx{ fd }
);
```

### 3. 携带多个字段的任务

`BgTask` 只能通过内联 `storage` 携带一份参数结构体（≤ 32 字节）。
需要传多个字段时，定义一个继承 `BgTask` 的派生类，并使用 `emplace<Data>()` 打包：

```cpp
#include "my_task.h"

struct ReceiveTask : BgTask {
    struct Data {
        int      fd;
        uint8_t* buf;
        size_t   len;
    };

    ReceiveTask(int fd, uint8_t* buf, size_t len) {
        emplace<Data>(fd, buf, len);
    }

    ~ReceiveTask() override {
        auto* d = get<Data>();
        if (d) {
            free(d->buf);
            destroy<Data>();
        }
    }

protected:
    void run() override {
        auto* d = get<Data>();
        // 处理 d->buf[0..d->len)
    }
};

// 提交
MyBackground::GetInstance().Schedule<ReceiveTask>(
    "Recv", fd, buf, len);
```

**约束**：`ReceiveTask` 的 `sizeof` 必须等于 `sizeof(BgTask)`，不能加成员变量。
所有字段都放进 `Data` 结构体，通过 `emplace<Data>()` 内联或堆分配。

## 调度策略

| 场景 | 行为 |
|------|------|
| 队列从空变非空 | 启动单次定时器，从此刻开始计时 |
| 队列达到 `CONFIG_BATCH_SIZE`（默认 16） | 立即唤醒后台，停定时器 |
| 定时器到期（`CONFIG_WAIT_MS`，默认 40ms） | 唤醒后台处理剩余零散任务 |
| 后台消费完队列 | 回睡；下一次任务到达时重新计时 |

批量唤醒的目的是**减少上下文切换次数**。事件率高时收益显著；事件率低时定时器兜底，
保证不会长期不处理。

## API

### `Schedule`

```cpp
bool Schedule(const char* name, RunFn fn, FreeFn free = nullptr, void* arg = nullptr);

template <typename T, typename... Args>
bool Schedule(const char* name, Args&&... args);
```

| 参数 | 说明 |
|------|------|
| `name` | 任务名，用于 `Clear()`。长度 ≤ `CONFIG_BG_NAME_LEN-1`，超长截断 |
| `fn` | 任务执行函数，签名 `void(void*)`。必须是**无捕获 lambda** 或函数指针 |
| `free` | 可选清理回调，签名 `void(void*, bool executed)`。任务销毁前调用 |
| `arg` | 传给 `fn` / `free` 的用户上下文 |

返回 `false` 表示队列已满，任务未提交。

### `Clear`

```cpp
size_t Clear(const std::string& name);
```

- `name` 为空 → 清空所有未开始执行的任务；
- 否则 → 删除所有 `name` 匹配的未开始执行的任务；
- 返回删除的数量。

**清理语义**：

- 正在执行的任务已经摘出队列，`Clear` 看不到它，不会被中断；
- 尚未开始执行的任务会被删除，其 `free` 回调会以 `executed=false` 调用；
- 已加入队列但未运行的任务，`free` 会被调用，保证资源不泄漏。

### `GetBackgroundTasks`

```cpp
size_t GetBackgroundTasks() const;
```

返回当前**待处理**任务数（不含正在运行的那个）。

## 线程安全

| 操作 | 线程安全 |
|------|---------|
| `Schedule` | ✅ 任意线程 |
| `Clear` | ✅ 任意线程 |
| `GetBackgroundTasks` | ✅ 任意线程 |
| `PrintBackgroundInfo` | ✅ 任意线程（本身就是一个任务） |

内部由 `MyList` 的互斥锁保护所有结构操作。用户回调（`run` / `free` / 派生类构造、
析构）一律在锁外执行，可以放心做耗时操作。

## 配置

在 `my_background.h` 中可调整：

| 宏 | 默认值 | 说明 |
|-----|-------|------|
| `CONFIG_BATCH_SIZE` | 16 | 攒够多少个任务立即唤醒后台 |
| `CONFIG_WAIT_MS` | 40 | 第一个任务到达后最长等待时间（ms） |
| `CONFIG_BG_NAME_LEN` | 10 | 任务名最大长度（含终止符） |
| `CONFIG_BG_INLINE_DATA_SIZE` | 32 | `BgTask::storage` 内联数据区大小 |

外部依赖（`sdkconfig.h`）：

| 宏 | 说明 |
|-----|------|
| `CONFIG_MAX_BACKGROUND_TASKS` | 后台任务队列容量 |
| `CONFIG_STACK_SIZE` | 后台任务栈大小（word 单位） |
| `CONFIG_BACKGROUND_TASKS_PRIORITY` | 后台任务优先级 |
| `CONFIG_CORE_ID` | 绑定核心，`-1` 表示不绑定 |

## 使用建议

### 1. 任务名要可识别

`Clear(name)` 依赖任务名精确匹配。建议按事件类型命名，如 `"TcpRecv"`、`"TcpSent"`、
`"TimerTick"`，避免用不同名字表示同一类任务，导致清理失效。

### 2. `free` 回调必须 noexcept

`free` 在 `BgTask` 析构中调用，析构函数是 `noexcept`。抛出会 `std::terminate`。
所有资源释放逻辑（`close`、`delete`、`pbuf_free` 等）必须保证不抛。

### 3. `Schedule` 失败时手动释放资源

```cpp
auto ok = MyBackground::GetInstance().Schedule("Recv", run, free, arg);
if (!ok) {
    // 队列满，手动执行 free 逻辑，避免泄漏
    free(arg, false);
}
```

### 4. 不要传入捕获 lambda

```cpp
// ❌ 编译失败：捕获 lambda 无法转为函数指针
MyBackground::GetInstance().Schedule("X", [&](void*) { ... });

// ✅ 无捕获 lambda
MyBackground::GetInstance().Schedule("X", [](void*) { ... });

// ✅ 需要捕获时，走 void* arg
struct Ctx { int a; };
MyBackground::GetInstance().Schedule(
    "X",
    [](void* arg) { auto* c = static_cast<Ctx*>(arg); /* 用 c->a */ },
    nullptr,
    new Ctx{42}
);
```

### 5. 长 Run 场景下避免在回调里调用 `Schedule`

回调自身运行在后台线程。如果回调里再 `Schedule` 且队列满，会阻塞等待，形成自锁。
若必须再调度，确保队列不会满，或者改用 `try` 语义包装。

## 完整示例

```cpp
// main.cpp
#include "my_background.h"
#include "esp_log.h"

static const char* TAG = "Demo";

// ---- 场景 1：日志打印 ----
static void LogTask(void* arg) {
    const char* msg = static_cast<const char*>(arg);
    ESP_LOGI(TAG, "task: %s", msg);
}

// ---- 场景 2：数据上报（带资源清理） ----
struct ReportData {
    int     id;
    uint8_t payload[64];
};

static void ReportRun(void* arg) {
    auto* d = static_cast<ReportData*>(arg);
    ESP_LOGI(TAG, "report id=%d", d->id);
    // ... 编码、发送 ...
}

static void ReportFree(void* arg, bool executed) {
    auto* d = static_cast<ReportData*>(arg);
    if (!executed) {
        ESP_LOGW(TAG, "report id=%d dropped", d->id);
    }
    delete d;
}

extern "C" void app_main(void)
{
    auto& bg = MyBackground::GetInstance();

    // 提交无参任务
    bg.Schedule("Log1", LogTask, nullptr, (void*)"startup");

    // 提交带清理的任务
    bg.Schedule("Report", ReportRun, ReportFree, new ReportData{ .id = 1 });

    // 模拟突发：连续提交 20 个任务，攒够批处理阈值
    for (int i = 0; i < 20; i++) {
        bg.Schedule("Log1", LogTask, nullptr, (void*)"burst");
    }

    // 按名清理：删除所有名为 "Log1" 的未开始任务
    size_t n = bg.Clear("Log1");
    ESP_LOGI(TAG, "cleared %u Log1 tasks", (unsigned)n);

    // 查看状态
    bg.PrintBackgroundInfo();

    // 保持主任务存活
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
```

## 常见问题

**Q：队列满了怎么办？**

`Schedule` 返回 `false`。调用方需要决定：丢弃、等待、还是走旁路逻辑。
建议在 `Schedule` 失败时同步执行 `free` 逻辑，避免资源泄漏。

**Q：任务执行顺序是什么？**

FIFO。`Schedule` 顺序提交，后台按入队顺序执行。

**Q：`Clear` 会中断正在执行的任务吗？**

不会。正在执行的任务已从队列摘出，`Clear` 看不到它。只影响未开始的任务。

**Q：为什么 `Schedule` 不立即唤醒后台？**

批量唤醒可以减少上下文切换。攒够 `CONFIG_BATCH_SIZE` 或者等满 `CONFIG_WAIT_MS`
才唤醒。对延迟敏感的场景可以调小这两个值，或设为 `CONFIG_BATCH_SIZE = 1`。

**Q：`BgTask` 派生类为什么不能加成员变量？**

`MyList` 的槽位大小是 `sizeof(BgTask)` 固定值，`Schedule` 会在其上执行
placement new。派生类超出会越界写。所有数据必须放进 `Data` 结构体，通过
`emplace<Data>()` 打包。

**Q：任务能带多大参数？**

`Data` 结构体 ≤ `CONFIG_BG_INLINE_DATA_SIZE`（默认 32 字节）走内联，超过走堆分配。
对高频任务建议控制在 32 字节内，避免堆碎片。

**Q：可以在 ISR 里 `Schedule` 吗？**

`Schedule` 内部用了 `std::mutex`，不能在 ISR 里调用。ISR 里应通过队列通知或
task notify 转到普通线程后再 `Schedule`。