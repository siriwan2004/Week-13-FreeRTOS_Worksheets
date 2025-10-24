// step3_inversion_with_affinity.c — STEP3 + Exercise 2 (Priority, Round-Robin, Priority Inversion, Dual-Core Affinity)

#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stddef.h>              // สำหรับ NULL
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"

/* ==============================
 * CONFIG
 * ============================== */
// 0 = โชว์ปัญหา Priority Inversion (flag)
// 1 = แก้ด้วย Mutex + priority inheritance
#define INVERSION_FIXED_WITH_MUTEX  1

// Pins
#define LED_HIGH_PIN GPIO_NUM_16
#define LED_MED_PIN  GPIO_NUM_17
#define LED_LOW_PIN  GPIO_NUM_18
#define BUTTON_PIN   GPIO_NUM_23

static const char *TAG = "LAB_STEP3";

/* ==============================
 * Shared flags / stats
 * ============================== */
static volatile bool     priority_test_running = false;
static volatile uint32_t high_task_count = 0;
static volatile uint32_t med_task_count  = 0;
static volatile uint32_t low_task_count  = 0;

/* Priority Inversion resource */
#if INVERSION_FIXED_WITH_MUTEX
static SemaphoreHandle_t shared_mutex = NULL;
#else
static volatile bool shared_resource_busy = false;
#endif

/* ==============================
 * Utilities
 * ============================== */
static inline void busy_loop(uint32_t iters, bool pet_wdt)
{
    volatile uint32_t d = 0;
    for (uint32_t i = 0; i < iters; i++) {
        d += i ^ (i << 1);
        if (pet_wdt && (i % 100000 == 0)) vTaskDelay(1);
    }
}

/* ==============================
 * STEP 1 — Basic Priority Demo
 * ============================== */
static void high_priority_task(void *pv)
{
    ESP_LOGI(TAG, "High Priority Task started (prio=5)");
    while (1) {
        if (priority_test_running) {
            high_task_count++;
            gpio_set_level(LED_HIGH_PIN, 1);
            busy_loop(100000, false);
            gpio_set_level(LED_HIGH_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static void medium_priority_task(void *pv)
{
    ESP_LOGI(TAG, "Medium Priority Task started (prio=3)");
    while (1) {
        if (priority_test_running) {
            med_task_count++;
            gpio_set_level(LED_MED_PIN, 1);
            busy_loop(200000, false);
            gpio_set_level(LED_MED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(300));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static void low_priority_task(void *pv)
{
    ESP_LOGI(TAG, "Low Priority Task started (prio=1)");
    while (1) {
        if (priority_test_running) {
            low_task_count++;
            gpio_set_level(LED_LOW_PIN, 1);
            busy_loop(500000, true);
            gpio_set_level(LED_LOW_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

/* Control task */
static void control_task(void *pv)
{
    ESP_LOGI(TAG, "Control Task started (prio=4)");
    bool latch = false;

    while (1) {
        int level = gpio_get_level(BUTTON_PIN);
        if (level == 0 && !latch) {
            latch = true;

            ESP_LOGW(TAG, "=== START TEST (10s) ===");
            high_task_count = med_task_count = low_task_count = 0;
            priority_test_running = true;
            vTaskDelay(pdMS_TO_TICKS(10000));
            priority_test_running = false;

            ESP_LOGW(TAG, "=== RESULTS ===");
            ESP_LOGI(TAG, "High runs:   %" PRIu32, high_task_count);
            ESP_LOGI(TAG, "Medium runs: %" PRIu32, med_task_count);
            ESP_LOGI(TAG, "Low runs:    %" PRIu32, low_task_count);
            uint32_t total = high_task_count + med_task_count + low_task_count;
            if (total) {
                ESP_LOGI(TAG, "High  %%: %.1f", (100.0f * high_task_count) / total);
                ESP_LOGI(TAG, "Medium%%: %.1f", (100.0f * med_task_count)  / total);
                ESP_LOGI(TAG, "Low   %%: %.1f", (100.0f * low_task_count)   / total);
            }
            ESP_LOGI(TAG, "Press button again to rerun.");
        }
        if (level == 1) latch = false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ==============================
 * STEP 2 — Round-Robin Demo
 * ============================== */
static inline void eq_work_slice(void)
{
    for (int i = 0; i < 300000; i++) { volatile int dummy=i; (void)dummy; }
}

static void equal_priority_task(void *pvParameters)
{
    int task_id = (int)(intptr_t)pvParameters;
    ESP_LOGI(TAG, "[EQ%d] started (prio=2)", task_id);

    while (1) {
        if (priority_test_running) {
            ESP_LOGI(TAG, "[EQ%d] running", task_id);
            eq_work_slice();
        }
        vTaskDelay(1);
    }
}

/* ==============================
 * STEP 3 — Priority Inversion Demo
 * ============================== */
static void inv_medium_cpu(void *pvParameters)
{
    ESP_LOGI(TAG, "INV Medium started (prio=4)");
    while (1) {
        if (priority_test_running) {
            for (int r=0;r<4;r++){
                busy_loop(250000,true);
                taskYIELD();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void inv_high(void *pvParameters)
{
    ESP_LOGI(TAG, "INV High started (prio=5)");
    while (1) {
        if (priority_test_running) {
            ESP_LOGW(TAG,"High needs shared resource");
#if INVERSION_FIXED_WITH_MUTEX
            xSemaphoreTake(shared_mutex, portMAX_DELAY);
            ESP_LOGI(TAG,"High got resource (mutex)");
            vTaskDelay(pdMS_TO_TICKS(100));
            xSemaphoreGive(shared_mutex);
#else
            while (shared_resource_busy) { ESP_LOGW(TAG,"High BLOCKED by Low"); vTaskDelay(pdMS_TO_TICKS(10)); }
            ESP_LOGI(TAG,"High got resource (flag)");
#endif
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void inv_low(void *pvParameters)
{
    ESP_LOGI(TAG, "INV Low started (prio=1)");
    while (1) {
        if (priority_test_running) {
#if INVERSION_FIXED_WITH_MUTEX
            xSemaphoreTake(shared_mutex, portMAX_DELAY);
            ESP_LOGI(TAG,"Low uses resource (mutex) long work...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            xSemaphoreGive(shared_mutex);
#else
            shared_resource_busy=true;
            ESP_LOGI(TAG,"Low uses resource (flag) long work...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            shared_resource_busy=false;
#endif
            ESP_LOGI(TAG,"Low released resource");
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

/* ==============================
 * Exercise 2 — Dual-Core Affinity Demo
 * ============================== */
static void ex2_high_affinity(void *pv)
{
    while(1){
        ESP_LOGI(TAG,"[Ex2 HighPrio] running on Core %d", xPortGetCoreID());
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void ex2_low_affinity(void *pv)
{
    while(1){
        ESP_LOGI(TAG,"[Ex2 LowPrio] running on Core %d", xPortGetCoreID());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ==============================
 * Main
 * ============================== */
void app_main(void)
{
    ESP_LOGI(TAG, "=== STEP3 + Exercise2: Priority, Round-Robin, Priority Inversion, Dual-Core Affinity ===");

    // LEDs
    gpio_config_t leds = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL<<LED_HIGH_PIN)|(1ULL<<LED_MED_PIN)|(1ULL<<LED_LOW_PIN),
        .pull_down_en=0,
        .pull_up_en=0
    };
    gpio_config(&leds);
    gpio_set_level(LED_HIGH_PIN,0);
    gpio_set_level(LED_MED_PIN,0);
    gpio_set_level(LED_LOW_PIN,0);

    // Button pull-up
    gpio_config_t btn = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL<<BUTTON_PIN),
        .pull_down_en=0,
        .pull_up_en=1
    };
    gpio_config(&btn);

#if INVERSION_FIXED_WITH_MUTEX
    shared_mutex = xSemaphoreCreateMutex();
#endif

    // STEP1 Tasks
    xTaskCreatePinnedToCore(high_priority_task, "HighPrio", 3072,NULL,5,NULL,0);
    xTaskCreatePinnedToCore(medium_priority_task, "MedPrio",3072,NULL,3,NULL,0);
    xTaskCreatePinnedToCore(low_priority_task, "LowPrio",3072,NULL,1,NULL,1);
    xTaskCreate(control_task,"Control",3072,NULL,4,NULL);

    // STEP2 Tasks
    xTaskCreatePinnedToCore(equal_priority_task,"EQ1",2048,(void*)1,2,NULL,0);
    xTaskCreatePinnedToCore(equal_priority_task,"EQ2",2048,(void*)2,2,NULL,0);
    xTaskCreatePinnedToCore(equal_priority_task,"EQ3",2048,(void*)3,2,NULL,0);

    // STEP3 Tasks
    xTaskCreatePinnedToCore(inv_high,"InvHigh",3072,NULL,5,NULL,0);
    xTaskCreatePinnedToCore(inv_medium_cpu,"InvMed",3072,NULL,4,NULL,0);
    xTaskCreatePinnedToCore(inv_low,"InvLow",3072,NULL,1,NULL,0);

    // Exercise2 Dual-Core Affinity
    xTaskCreatePinnedToCore(ex2_high_affinity,"HighPrio_Affinity",3072,NULL,5,NULL,0);
    xTaskCreatePinnedToCore(ex2_low_affinity,"LowPrio_Affinity",3072,NULL,1,NULL,1);

   ESP_LOGI(TAG,"Press BUTTON(GPIO%d) to start 10s test. LEDs:16=High,17=Med,18=Low", BUTTON_PIN);
}