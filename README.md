# my-background

一个后台管理任务，主要是消除在ESP32等平台上由于创建任务造成的内存浪费、进程切换开销。

## 功能
- 调试支持
    1. 打印最大后台运行任务数量;
    2. 打印系统中各任务信息（需配置开启 USE_STATS_FORMATTING_FUNCTIONS 选项）
- 调度任务至后台运行
    1. 支持无参数任务；
    2. 支持含参数任务；
    3. 默认支持后台任务函数捕获；
- 适用情况
    1. 弱实时性任务（暂未支持优先调度）
    2. 栈消耗怪兽
    3. 不建议轻CPU、低栈内存任务（性能降低，建议直接在函数中执行）


## To do
1. 考虑批量获取任务后无锁运行（作为配置可选）

## 示例
```cpp
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "my_led.h"
#include "my_background.h"

#define TAG "MAIN"

extern "C" void app_main(void)
{
    MyLed led12(GPIO_NUM_12);
    MyLed led13(GPIO_NUM_13);

    led12.StartContinuousBlink();


    led13.EnableBrightnessAdjust(true, LEDC_CHANNEL_1, LEDC_TIMER_1);
    // led13.StartContinuousBlink(500);
    led13.SetBrightness(7);
    auto& background = MyBackground::GetInstance();
    int size=0;

    while(1) {
        background.Schedule([](){
            ESP_LOGE(TAG, "Run in background");
        });
        background.Schedule([](void* arg){
                ESP_LOGW(TAG, "size=%d", *static_cast<int*>(arg));
            }, 
            &size
        );
        vTaskDelay(pdMS_TO_TICKS(10*1000));
        led13.SetBrightness(0);
        led12.TurnOn();
        size++;
    }
}
```