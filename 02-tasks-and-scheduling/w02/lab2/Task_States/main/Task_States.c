#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"

// ===== Pins =====
#define LED_RUNNING    GPIO_NUM_2
#define LED_READY      GPIO_NUM_4
#define LED_BLOCKED    GPIO_NUM_5
#define LED_SUSPENDED  GPIO_NUM_18

#define BUTTON1_PIN    GPIO_NUM_0    // Suspend/Resume
#define BUTTON2_PIN    GPIO_NUM_35   // Give semaphore (input-only, ไม่มี pull-up)

static const char *TAG = "EX2_STATE_INDICATOR";

// ===== Handles =====
static TaskHandle_t state_demo_task_handle   = NULL;
static TaskHandle_t ready_state_demo_handle  = NULL;
static TaskHandle_t control_task_handle      = NULL;
static TaskHandle_t external_delete_handle   = NULL;

// ===== Sync =====
static SemaphoreHandle_t demo_semaphore = NULL;

// ===== State names =====
static const char* state_names[] = {
    "Running","Ready","Blocked","Suspended","Deleted","Invalid"
};
static inline const char* get_state_name(eTaskState s){
    return (s <= eDeleted) ? state_names[s] : state_names[5];
}

// ===== Helpers =====
static inline void leds_off(void){
    gpio_set_level(LED_RUNNING,   0);
    gpio_set_level(LED_READY,     0);
    gpio_set_level(LED_BLOCKED,   0);
    gpio_set_level(LED_SUSPENDED, 0);
}

// Exercise 2: Custom State Indicator
static void update_state_display(eTaskState current_state)
{
    leds_off();
    switch (current_state) {
        case eRunning:   gpio_set_level(LED_RUNNING,   1); break;
        case eReady:     gpio_set_level(LED_READY,     1); break;
        case eBlocked:   gpio_set_level(LED_BLOCKED,   1); break;
        case eSuspended: gpio_set_level(LED_SUSPENDED, 1); break;
        default:
            for (int i = 0; i < 3; i++) {
                gpio_set_level(LED_RUNNING, 1);
                gpio_set_level(LED_READY, 1);
                gpio_set_level(LED_BLOCKED, 1);
                gpio_set_level(LED_SUSPENDED, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                leds_off();
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            break;
    }
}

static inline void transition_state(eTaskState *cur, eTaskState next) {
    if (*cur != next) {
        ESP_LOGI(TAG, "Transition: %s -> %s", get_state_name(*cur), get_state_name(next));
        *cur = next;
        update_state_display(next);
    } else {
        update_state_display(next);
    }
}

/* ================================
 * Tasks
 * ================================ */
static void state_demo_task(void *pv)
{
    ESP_LOGI(TAG, "State Demo Task started (prio=3)");
    int cycle = 0;
    eTaskState cur = eReady; // เริ่มต้นก่อนถูก schedule
    transition_state(&cur, eRunning);

    while (1) {
        cycle++;
        ESP_LOGI(TAG, "=== Cycle %d ===", cycle);

        // RUNNING: heavy work
        transition_state(&cur, eRunning);
        for (int i = 0; i < 1000000; i++) {
            volatile int d = i * 2; (void)d;
            if ((i & 0x3FFFF) == 0) taskYIELD();
        }

        // READY: yield ให้ task prio เท่ากัน
        transition_state(&cur, eReady);
        taskYIELD();
        // กลับมา RUNNING
        transition_state(&cur, eRunning);

        // BLOCKED: รอ semaphore
        transition_state(&cur, eBlocked);
        if (xSemaphoreTake(demo_semaphore, pdMS_TO_TICKS(2000)) == pdTRUE) {
            transition_state(&cur, eRunning);
            ESP_LOGI(TAG, "Got semaphore -> short RUNNING then delay(500)");
            transition_state(&cur, eBlocked);
            vTaskDelay(pdMS_TO_TICKS(500));
            transition_state(&cur, eRunning);
        } else {
            ESP_LOGI(TAG, "Semaphore timeout");
        }

        // BLOCKED: vTaskDelay
        transition_state(&cur, eBlocked);
        vTaskDelay(pdMS_TO_TICKS(1000));
        transition_state(&cur, eRunning);
    }
}

static void ready_state_demo_task(void *pv)
{
    ESP_LOGI(TAG, "Ready State Demo started (prio=3)");
    while (1) {
        for (int i = 0; i < 100000; i++) { volatile int d = i; (void)d; }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static void self_deleting_task(void *pv)
{
    int *lt = (int*)pv; int t = lt ? *lt : 10;
    ESP_LOGI(TAG, "Self-delete lifetime = %d s", t);
    for (int i = t; i > 0; --i) {
        ESP_LOGI(TAG, "Self-delete countdown: %d", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Self-delete -> DELETED");
    vTaskDelete(NULL);
}

static void external_delete_task(void *pv)
{
    int c = 0;
    ESP_LOGI(TAG, "External-delete task started (prio=2)");
    while (1) {
        ESP_LOGI(TAG, "ExtDelete running: %d", c++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void control_task(void *pv)
{
    ESP_LOGI(TAG, "Control Task started (prio=4)");
    bool suspended = false;
    int  ticks_100ms = 0;
    static bool external_deleted = false;

    while (1) {
        // Suspend/Resume StateDemo
        if (gpio_get_level(BUTTON1_PIN) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (!suspended) {
                // ไป Suspended
                transition_state(&(eTaskState){eRunning}, eSuspended); // แสดงผลที่ LED
                vTaskSuspend(state_demo_task_handle);
                suspended = true;
                ESP_LOGW(TAG, "=== SUSPEND StateDemo ===");
            } else {
                // จาก Suspended -> Ready (จะถูกจัดคิว)
                transition_state(&(eTaskState){eSuspended}, eReady);
                vTaskResume(state_demo_task_handle);
                suspended = false;
                ESP_LOGW(TAG, "=== RESUME StateDemo ===");
            }
            while (gpio_get_level(BUTTON1_PIN) == 0) vTaskDelay(pdMS_TO_TICKS(10));
        }

        // Give semaphore
        if (gpio_get_level(BUTTON2_PIN) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            ESP_LOGW(TAG, "=== GIVE SEMAPHORE ===");
            xSemaphoreGive(demo_semaphore);
            while (gpio_get_level(BUTTON2_PIN) == 0) vTaskDelay(pdMS_TO_TICKS(10));
        }

        // Delete external task after ~15s
        if (!external_deleted && ticks_100ms >= 150) {
            if (external_delete_handle) {
                ESP_LOGW(TAG, "Deleting external task");
                vTaskDelete(external_delete_handle);
                external_deleted = true;
            }
        }

        ticks_100ms++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ================================
 * app_main
 * ================================ */
void app_main(void)
{
    ESP_LOGI(TAG, "=== Exercise 2: Custom State Indicator ===");

    // LEDs
    gpio_config_t leds = {
        .intr_type = GPIO_INTR_DISABLE, .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED_RUNNING) | (1ULL << LED_READY) |
                        (1ULL << LED_BLOCKED) | (1ULL << LED_SUSPENDED),
        .pull_down_en = 0, .pull_up_en = 0,
    };
    gpio_config(&leds);
    leds_off();

    // BUTTON1 (GPIO0)
    gpio_config_t btn1 = {
        .intr_type = GPIO_INTR_DISABLE, .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BUTTON1_PIN),
        .pull_up_en = 1, .pull_down_en = 0,
    };
    gpio_config(&btn1);

    // BUTTON2 (GPIO35) — ไม่มี pull-up ภายใน
    gpio_config_t btn2 = {
        .intr_type = GPIO_INTR_DISABLE, .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BUTTON2_PIN),
        .pull_up_en = 0, .pull_down_en = 0,
    };
    gpio_config(&btn2);

    // Binary semaphore
    demo_semaphore = xSemaphoreCreateBinary();
    if (!demo_semaphore) { ESP_LOGE(TAG, "Failed to create semaphore"); return; }

    // Tasks
    xTaskCreate(state_demo_task,       "StateDemo", 4096, NULL, 3, &state_demo_task_handle);
    xTaskCreate(ready_state_demo_task, "ReadyDemo", 2048, NULL, 3, &ready_state_demo_handle);
    xTaskCreate(control_task,          "Control",   3072, NULL, 4, &control_task_handle);

    static int self_delete_time = 10;
    xTaskCreate(self_deleting_task,    "SelfDelete", 2048, &self_delete_time, 2, NULL);
    xTaskCreate(external_delete_task,  "ExtDelete",  2048, NULL,               2, &external_delete_handle);

    ESP_LOGI(TAG, "LED: 2=Running, 4=Ready, 5=Blocked, 18=Suspended");
    ESP_LOGI(TAG, "BTN: GPIO0=Susp/Resume, GPIO35=Semaphore (need external pull-up)");
    }