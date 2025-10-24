#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_log.h"

#define LED1_PIN GPIO_NUM_2
#define LED2_PIN GPIO_NUM_4
static const char *TAG = "STEP3_EX3";

#define PRINT_HEAP(tag, msg, val) \
    ESP_LOGI(tag, msg " %" PRIu32 " bytes", (uint32_t)(val))

// ---------- Step 1: Basic tasks ----------
void led1_task(void *pvParameters)
{
    int *task_id = (int *)pvParameters;
    ESP_LOGI(TAG, "LED1 Task started with ID: %d", *task_id);
    while (1)
    {
        ESP_LOGI(TAG, "LED1 ON");
        gpio_set_level(LED1_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI(TAG, "LED1 OFF");
        gpio_set_level(LED1_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void led2_task(void *pvParameters)
{
    char *task_name = (char *)pvParameters;
    ESP_LOGI(TAG, "LED2 Task started: %s", task_name);
    while (1)
    {
        ESP_LOGI(TAG, "LED2 Blink Fast");
        for (int i = 0; i < 5; i++)
        {
            gpio_set_level(LED2_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED2_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void system_info_task(void *pvParameters)
{
    ESP_LOGI(TAG, "System Info Task started");
    while (1)
    {
        ESP_LOGI(TAG, "=== System Information ===");
        PRINT_HEAP(TAG, "Free heap:", esp_get_free_heap_size());
        PRINT_HEAP(TAG, "Min free heap:", esp_get_minimum_free_heap_size());
        UBaseType_t n = uxTaskGetNumberOfTasks();
        ESP_LOGI(TAG, "Number of tasks: %lu", (unsigned long)n);
        TickType_t tick = xTaskGetTickCount();
        uint32_t up = (uint32_t)(tick * portTICK_PERIOD_MS / 1000);
        ESP_LOGI(TAG, "Uptime: %" PRIu32 " seconds", up);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// ---------- Step 2: Task Manager ----------
static const char *state_to_str(eTaskState s)
{
    switch (s)
    {
    case eRunning:
        return "Running";
    case eReady:
        return "Ready";
    case eBlocked:
        return "Blocked";
    case eSuspended:
        return "Suspended";
    case eDeleted:
        return "Deleted";
    default:
        return "Unknown";
    }
}

void task_manager(void *pvParameters)
{
    ESP_LOGI(TAG, "Task Manager started");
    TaskHandle_t *handles = (TaskHandle_t *)pvParameters;
    TaskHandle_t led1_handle = handles[0];
    TaskHandle_t led2_handle = handles[1];

    int cmd = 0;
    while (1)
    {
        cmd++;
        switch (cmd % 6)
        {
        case 1:
            ESP_LOGI(TAG, "Manager: Suspending LED1");
            vTaskSuspend(led1_handle);
            break;
        case 2:
            ESP_LOGI(TAG, "Manager: Resuming LED1");
            vTaskResume(led1_handle);
            break;
        case 3:
            ESP_LOGI(TAG, "Manager: Suspending LED2");
            vTaskSuspend(led2_handle);
            break;
        case 4:
            ESP_LOGI(TAG, "Manager: Resuming LED2");
            vTaskResume(led2_handle);
            break;
        case 5:
        {
            eTaskState s1 = eTaskGetState(led1_handle);
            eTaskState s2 = eTaskGetState(led2_handle);
            ESP_LOGI(TAG, "LED1 State: %s", state_to_str(s1));
            ESP_LOGI(TAG, "LED2 State: %s", state_to_str(s2));
            break;
        }
        case 0:
            ESP_LOGI(TAG, "Manager: Reset cycle");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// ---------- Step 3: Priorities + runtime stats ----------
void high_priority_task(void *pvParameters)
{
    ESP_LOGI(TAG, "High Priority Task started");
    while (1)
    {
        ESP_LOGW(TAG, "HIGH PRIORITY TASK RUNNING!");
        for (int i = 0; i < 1000000; i++)
        {
            volatile int dummy = i;
            (void)dummy;
        }
        ESP_LOGW(TAG, "High priority task yielding");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void low_priority_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Low Priority Task started");
    while (1)
    {
        ESP_LOGI(TAG, "Low priority task running");
        for (int i = 0; i < 100; i++)
        {
            ESP_LOGI(TAG, "Low priority work: %d/100", i + 1);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

void runtime_stats_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Runtime Stats Task started");
    char *buffer = (char *)malloc(2048);
    if (!buffer)
    {
        ESP_LOGE(TAG, "Failed to allocate buffer for runtime stats");
        vTaskDelete(NULL);
        return;
    }
    while (1)
    {
        ESP_LOGI(TAG, "\n=== Runtime Statistics ===");
        vTaskGetRunTimeStats(buffer);
        ESP_LOGI(TAG, "Task\t\tAbs Time\tPercent Time\n%s", buffer);

        ESP_LOGI(TAG, "\n=== Task List ===");
        vTaskList(buffer);
        ESP_LOGI(TAG, "Name\t\tState\tPrio\tStack\tNum\n%s", buffer);

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
    free(buffer);
    vTaskDelete(NULL);
}

// ---------- Exercise 3: Queue Communication ----------
QueueHandle_t sensorQueue;

void sensor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Sensor Task started");
    int data = 0;
    while (1)
    {
        data = rand() % 100; // จำลองการอ่านค่าจากเซนเซอร์
        if (xQueueSend(sensorQueue, &data, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            ESP_LOGI(TAG, "Sensor sent data: %d", data);
        }
        else
        {
            ESP_LOGW(TAG, "Queue full! Sensor data lost");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void display_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Display Task started");
    int recv_data = 0;
    while (1)
    {
        if (xQueueReceive(sensorQueue, &recv_data, portMAX_DELAY))
        {
            ESP_LOGI(TAG, "Display received data: %d", recv_data);
        }
    }
}

// ---------- MAIN ----------
void app_main(void)
{
    ESP_LOGI(TAG, "=== Step 3 + Exercise 3 ===");

    // GPIO
    gpio_config_t io = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED1_PIN) | (1ULL << LED2_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io);

    // Step 1 tasks
    static int led1_id = 1;
    static char led2_name[] = "FastBlinker";
    TaskHandle_t h1 = NULL, h2 = NULL, hinfo = NULL;

    xTaskCreate(led1_task, "LED1_Task", 2048, &led1_id, 2, &h1);
    xTaskCreate(led2_task, "LED2_Task", 2048, led2_name, 2, &h2);
    xTaskCreate(system_info_task, "SysInfo_Task", 3072, NULL, 1, &hinfo);

    // Step 2
    TaskHandle_t pack[2] = {h1, h2};
    xTaskCreate(task_manager, "TaskManager", 2048, pack, 3, NULL);

    // Step 3
    xTaskCreate(high_priority_task, "HighPrio", 4096, NULL, 4, NULL);
    xTaskCreate(low_priority_task, "LowPrio", 3072, NULL, 1, NULL);
    xTaskCreate(runtime_stats_task, "RtStats", 4096, NULL, 1, NULL);

    // Exercise 3: Queue communication
    sensorQueue = xQueueCreate(5, sizeof(int)); // สร้างคิวเก็บข้อมูล 5 ค่า
    if (sensorQueue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create queue!");
    }
    else
    {
        xTaskCreate(sensor_task, "SensorTask", 2048, NULL, 2, NULL);
        xTaskCreate(display_task, "DisplayTask", 2048, NULL, 2, NULL);
    }

    while (1)
    {
        PRINT_HEAP(TAG, "Free heap:", esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
