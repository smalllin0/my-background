# my-background

ESP32 轻量级后台任务调度组件。基于 `MyList` 固定容量对象池，
支持零堆分配的任务调度、批量唤醒、按名清理。适合从 lwIP 回调、
高优先级业务代码等上下文快速提交任务，在单独的后台任务中串行执行。

## 特性

- **零动态内存**：任务槽位编译期固定；`Data ≤ CONFIG_BG_INLINE_DATA_SIZE`
  时内联存储，不触发 `new`
- **锁外构造 / 析构**：构造、执行、清理都在 `MyList` 锁外完成
- **批量唤醒**：攒够 `CONFIG_BG_BATCH_SIZE` 或等满 `CONFIG_BG_WAIT_MS`
  才唤醒后台，减少上下文切换
- **可清理**：`Clear(name)` 按任务名精确删除尚未执行的任务
- **线程安全**：`Schedule` / `Clear` 可从任意线程调用

## 文件结构

```
my-background/
├── include/
│   ├── my_background.h   # MyBackground 类
│   └── my_task.h         # BgTask / TaskWrapper
├── my_background.cc
├── Kconfig
└── README.md
```

依赖：`my-linklist`（提供 `MyList<T, N>`）。

## 快速上手

### 1. 提交无参任务

```cpp
#include "my_background.h"

MyBackground::GetInstance().Schedule(
    "LogHello",                          // 任务名
    [](void*) {                          // 无捕获 lambda / 函数指针
        ESP_LOGI("App", "hello from background");
    }
);
```

### 2. 携带上下文与清理回调

```cpp
struct MyCtx { int fd; };

void OnDone(void* arg, bool executed) {
    auto* ctx = static_cast<MyCtx*>(arg);
    if (!executed) {
        ESP_LOGW(TAG, "task dropped");
    }
    delete ctx;
}

MyBackground::GetInstance().Schedule(
    "HandleConn",
    [](void* arg) {
        auto* ctx = static_cast<MyCtx*>(arg);
        // 执行耗时逻辑
    },
    OnDone,                              // 可选清理回调
    new MyCtx{ fd }
);
```

### 3. 携带多字段的派生任务

`BgTask` 只能通过内联 `storage` 携带一份参数结构体。需要传多个
字段时，定义派生类并用 `emplace<Data>()` 打包：

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
        // 只写业务清理；Data 的析构由基类自动完成
        auto* d = get<Data>();
        if (d && d->buf) {
            free(d->buf);
        }
    }

protected:
    void Run() override {
        auto* d = get<Data>();
        // 处理 d->buf[0..d->len)
    }
};

MyBackground::GetInstance().Schedule<ReceiveTask>(
    "Recv", fd, buf, len);
```

**约束**：派生类不能添加任何数据成员，`sizeof(派生类)` 必须
等于 `sizeof(BgTask)`。所有字段放入 `Data`，由 `emplace<Data>()`
内联或堆分配。

## 调度策略

| 场景 | 行为 |
|------|------|
| 队列从空变非空 | 启动单次定时器，从此刻开始计时 |
| 队列达到 `CONFIG_BG_BATCH_SIZE` | 立即唤醒后台，停掉定时器 |
| 定时器到期（`CONFIG_BG_WAIT_MS`） | 唤醒后台处理剩余零散任务 |
| 后台消费完队列 | 回睡；下个任务到达时重新计时 |

## API

### `Schedule`

```cpp
bool Schedule(const char* name, RunFn fn, FreeFn free = nullptr, void* arg = nullptr);

template <typename T, typename... Args>
bool Schedule(const char* name, Args&&... args);
```

| 参数 | 说明 |
|------|------|
| `name` | 任务名，用于 `Clear()`。长度 ≤ `CONFIG_BG_NAME_LEN - 1` |
| `fn` | 任务执行函数，`void(void*)`，必须无捕获 |
| `free` | 清理回调，`void(void*, bool executed)`，析构前调用 |
| `arg` | 传给 `fn` / `free` 的用户上下文 |

返回 `false` 表示队列已满或后台任务创建失败，任务未提交。

### `Clear`

```cpp
size_t Clear(const std::string& name);
```

- `name` 为空 → 清空所有未执行任务；
- 否则 → 删除所有名字匹配的未执行任务；
- 返回删除数量。

**清理语义**：

- 正在执行的任务已摘出队列，`Clear` 看不到；
- 尚未执行的任务被删除，`free(arg, false)` 被调用；
- 已加入队列但未运行的任务保证 `free` 被调用，不泄漏资源。

### `GetBackgroundTasks`

```cpp
size_t GetBackgroundTasks() const;
```

返回当前待处理任务数（不含正在运行的那一个）。

### `PrintBackgroundInfo`

```cpp
void PrintBackgroundInfo();
```

打印当前 / 峰值任务数、栈水位、可选的任务列表快照
（需要 `CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS`）。

## 配置项

通过 `idf.py menuconfig` → `MyBackground 组件配置` 修改：

| 宏 | 默认值 | 说明 |
|-----|-------|------|
| `CONFIG_MAX_BACKGROUND_TASKS` | 64 | 队列容量（槽位数） |
| `CONFIG_BG_BATCH_SIZE` | 16 | 批处理阈值 |
| `CONFIG_BG_WAIT_MS` | 40 | 最长等待时间（ms） |
| `CONFIG_STACK_SIZE` | 10240 | 后台任务栈（字节） |
| `CONFIG_CORE_ID` | 0 | 绑定核心 |
| `CONFIG_BACKGROUND_TASKS_PRIORITY` | 5 | 后台任务优先级 |
| `CONFIG_BG_NAME_LEN` | 10 | 任务名最大长度（含 `\0`） |
| `CONFIG_BG_INLINE_DATA_SIZE` | 32 | `Data` 内联区大小（字节） |
| `CONFIG_BG_DISPLAY_TASK_HANDLE_COMPLETE` | n | 是否打印任务完成日志 |

## 使用建议

### 1. 任务名保持可识别

`Clear(name)` 依赖任务名精确匹配。建议按事件类型命名，
如 `"TcpRecv"`、`"TcpSent"`、`"TimerTick"`。

### 2. `free` 回调必须 noexcept

`free` 在 `BgTask` 析构中调用，而析构是 `noexcept` 的。
所有资源释放逻辑必须保证不抛异常。

### 3. `Schedule` 失败时手动释放资源

```cpp
auto ok = MyBackground::GetInstance().Schedule("Recv", run, free, arg);
if (!ok) {
    free(arg, false);   // 队列满，手动清理，避免泄漏
}
```

### 4. 只传无捕获 lambda

```cpp
// ❌ 编译失败：带捕获的 lambda 无法转成函数指针
MyBackground::GetInstance().Schedule("X", [&](void*) { ... });

// ✅ 无捕获
MyBackground::GetInstance().Schedule("X", [](void*) { ... });

// ✅ 需要捕获 → 走 arg
struct Ctx { int a; };
MyBackground::GetInstance().Schedule(
    "X",
    [](void* p) { auto* c = static_cast<Ctx*>(p); /* c->a */ },
    nullptr,
    new Ctx{42}
);
```

### 5. 后台任务内慎调 `Schedule`

如果某个任务的 `Run` 里再调 `Schedule` 且队列可能已满，
会陷入自我调度循环。尽量避免，或确认队列容量足够。

### 6. 栈水位监控

`BackgroundHandler` 里的 `Run` 会消耗后台任务栈。如果 `Run`
里调用了 `cJSON_Parse`、`std::string`、深层递归，务必保证
`CONFIG_STACK_SIZE` 足够。

可通过 `PrintBackgroundInfo` 查看 `uxTaskGetStackHighWaterMark`
返回的历史最低水位——低于 `CONFIG_STACK_SIZE / 8` 时告警。

## 常见问题

**Q：队列满了怎么办？**

`Schedule` 返回 `false`。调用方需决定丢弃、等待还是旁路。
建议同步执行 `free(arg, false)` 避免泄漏。

**Q：任务执行顺序是什么？**

FIFO。`Schedule` 顺序提交，后台按入队顺序执行。

**Q：`Clear` 会中断正在执行的任务吗？**

不会。正在执行的任务已从队列摘出，`Clear` 看不到它。

**Q：为什么 `Schedule` 不立即唤醒后台？**

批量唤醒减少上下文切换。攒够 `CONFIG_BG_BATCH_SIZE` 或等满
`CONFIG_BG_WAIT_MS` 才唤醒。延迟敏感场景可调小这两个值，
或设 `CONFIG_BG_BATCH_SIZE = 1` 退化为立即唤醒。

**Q：`BgTask` 派生类为什么不能加成员变量？**

`MyList` 槽位大小是 `sizeof(BgTask)` 固定值，`Schedule` 在其上
placement new。派生类超出会越界。所有数据放进 `Data`，通过
`emplace<Data>()` 打包。

**Q：`Data` 能带多大？**

≤ `CONFIG_BG_INLINE_DATA_SIZE`（默认 32 字节）走内联；
超过走堆分配。高频任务建议控制在 32 字节内，避免堆碎片。

**Q：可以在 ISR 里 `Schedule` 吗？**

不可以。`Schedule` 内部用了 `std::mutex`，ISR 里不能用。
ISR 应先通过 `xTaskNotifyFromISR` 转到普通线程，再 `Schedule`。

**Q：任务执行完 `Data` 怎么释放？**

`BgTask` 基类析构时通过 `destroy` 函数指针自动清理——
如果内联存储，调 `Data::~Data()`；如果堆存储，`delete`。
**子类析构里不需要手动调 `destroy`**，只需要写业务资源
（如 `pbuf_free`、`free`）的释放。

**Q：如果后台任务创建失败会怎样？**

`Schedule` 会返回 `false`——所有后续任务被拒绝，不再静默
堆积。此时系统通常已处于不可恢复状态，应由上层决定如何处理
（重启、降级、上报）。