#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_random.h"
#include "esp_system.h"

static const char *TAG = "TIMER_APPS_EXP4";

/* ====== PROTOTYPES ====== */
typedef enum {
    PATTERN_OFF = 0,
    PATTERN_SLOW_BLINK,
    PATTERN_FAST_BLINK,
    PATTERN_HEARTBEAT,
    PATTERN_SOS,
    PATTERN_RAINBOW,
    PATTERN_MAX
} led_pattern_t;

static void recovery_callback(TimerHandle_t timer);
static void change_led_pattern(led_pattern_t new_pattern);
static void pattern_timer_callback(TimerHandle_t timer);
static float read_sensor_value(void);
static void sensor_timer_callback(TimerHandle_t timer);
static void status_timer_callback(TimerHandle_t timer);
static void watchdog_timeout_callback(TimerHandle_t timer);
static void feed_watchdog_callback(TimerHandle_t timer);
static void set_pattern_leds(bool led1, bool led2, bool led3);
static void sensor_processing_task(void *parameter);
static void system_monitor_task(void *parameter);
static void init_hardware(void);
static void create_timers(void);
static void create_queues(void);
static void start_system(void);

/* ===== Pins ===== */
#define STATUS_LED       GPIO_NUM_2
#define WATCHDOG_LED     GPIO_NUM_4
#define PATTERN_LED_1    GPIO_NUM_5
#define PATTERN_LED_2    GPIO_NUM_18
#define PATTERN_LED_3    GPIO_NUM_19
#define SENSOR_POWER     GPIO_NUM_21
#define SENSOR_PIN       GPIO_NUM_22 /* not used as ADC */

/* ===== Periods ===== */
#define WATCHDOG_TIMEOUT_MS     5000
#define WATCHDOG_FEED_MS        2000
#define PATTERN_BASE_MS         500
#define SENSOR_SAMPLE_MS        1000
#define STATUS_UPDATE_MS        3000

typedef struct {
    float value;
    uint32_t timestamp;
    bool valid;
} sensor_data_t;

typedef struct {
    uint32_t watchdog_feeds;
    uint32_t watchdog_timeouts;
    uint32_t pattern_changes;
    uint32_t sensor_readings;
    uint32_t system_uptime_sec;
    bool system_healthy;
} system_health_t;

/* ===== Globals ===== */
static TimerHandle_t watchdog_timer;
static TimerHandle_t feed_timer;
static TimerHandle_t pattern_timer;
static TimerHandle_t sensor_timer;
static TimerHandle_t status_timer;

static QueueHandle_t sensor_queue;
static QueueHandle_t pattern_queue;

static led_pattern_t current_pattern = PATTERN_OFF;
static int pattern_step = 0;
static system_health_t health_stats = {0, 0, 0, 0, 0, true};

typedef struct {
    int step;
    int direction;
    int intensity;
    bool state;
} pattern_state_t;

static pattern_state_t pattern_state = {0, 1, 0, false};

/* ADC calibration */
static esp_adc_cal_characteristics_t *adc_chars;

/* ================ WATCHDOG ================ */
static void watchdog_timeout_callback(TimerHandle_t timer) {
    health_stats.watchdog_timeouts++;
    health_stats.system_healthy = false;

    ESP_LOGE(TAG, "🚨 WATCHDOG TIMEOUT! Feeds=%lu Timeouts=%lu",
             health_stats.watchdog_feeds, health_stats.watchdog_timeouts);

    for (int i = 0; i < 10; i++) {
        gpio_set_level(WATCHDOG_LED, 1); vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(WATCHDOG_LED, 0); vTaskDelay(pdMS_TO_TICKS(50));
    }

    xTimerReset(watchdog_timer, 0);
    /* ไม่ restart ทันที ให้ health = false จนกว่าจะ recover */
}

static void recovery_callback(TimerHandle_t timer) {
    ESP_LOGI(TAG, "🔄 Recovery done, resume feed");
    health_stats.system_healthy = true;
    xTimerStart(feed_timer, 0);
    xTimerDelete(timer, 0);
}

static void feed_watchdog_callback(TimerHandle_t timer) {
    static int feed_count = 0; feed_count++;

    if (feed_count == 15) {
        ESP_LOGW(TAG, "🐛 Simulate hang 8s");
        xTimerStop(feed_timer, 0);
        TimerHandle_t recovery_timer = xTimerCreate("Recovery", pdMS_TO_TICKS(8000),
                                                    pdFALSE, (void*)0, recovery_callback);
        if (recovery_timer) xTimerStart(recovery_timer, 0);
        return;
    }

    health_stats.watchdog_feeds++;
    xTimerReset(watchdog_timer, 0);

    gpio_set_level(STATUS_LED, 1); vTaskDelay(pdMS_TO_TICKS(40));
    gpio_set_level(STATUS_LED, 0);
}

/* ================ PATTERN ================ */
static void set_pattern_leds(bool led1, bool led2, bool led3) {
    gpio_set_level(PATTERN_LED_1, led1);
    gpio_set_level(PATTERN_LED_2, led2);
    gpio_set_level(PATTERN_LED_3, led3);
}

static void change_led_pattern(led_pattern_t new_pattern) {
    const char* names[] = {"OFF","SLOW","FAST","HEARTBEAT","SOS","RAINBOW"};
    ESP_LOGI(TAG, "🎨 Pattern: %s -> %s", names[current_pattern], names[new_pattern]);

    current_pattern = new_pattern;
    pattern_step = 0;
    pattern_state.step = 0;
    pattern_state.state = false;
    health_stats.pattern_changes++;

    xTimerReset(pattern_timer, 0);
}

static void pattern_timer_callback(TimerHandle_t timer) {
    switch (current_pattern) {
        case PATTERN_OFF:
            set_pattern_leds(0,0,0);
            xTimerChangePeriod(timer, pdMS_TO_TICKS(800), 0);
            break;
        case PATTERN_SLOW_BLINK:
            pattern_state.state = !pattern_state.state;
            set_pattern_leds(pattern_state.state, 0, 0);
            xTimerChangePeriod(timer, pdMS_TO_TICKS(1000), 0);
            break;
        case PATTERN_FAST_BLINK:
            pattern_state.state = !pattern_state.state;
            set_pattern_leds(0, pattern_state.state, 0);
            xTimerChangePeriod(timer, pdMS_TO_TICKS(200), 0);
            break;
        case PATTERN_HEARTBEAT: {
            int st = pattern_step++ % 10;
            bool pulse = (st < 2) || (st >= 3 && st < 5);
            set_pattern_leds(0, 0, pulse);
            xTimerChangePeriod(timer, pdMS_TO_TICKS(100), 0);
        } break;
        case PATTERN_SOS: {
            static const char* sos = "...---...";
            static int pos = 0;
            bool dot = (sos[pos] == '.');
            int dur = dot ? 200 : 600;
            set_pattern_leds(1,1,1);
            vTaskDelay(pdMS_TO_TICKS(dur));
            set_pattern_leds(0,0,0);
            pos = (pos + 1) % (int)strlen(sos);
            xTimerChangePeriod(timer, pdMS_TO_TICKS(200), 0);
        } break;
        case PATTERN_RAINBOW: {
            int st = pattern_step++ % 8;
            set_pattern_leds(st & 1, st & 2, st & 4);
            xTimerChangePeriod(timer, pdMS_TO_TICKS(300), 0);
        } break;
        default: set_pattern_leds(0,0,0); break;
    }
}

/* ================ SENSOR ================ */
static float read_sensor_value(void) {
    gpio_set_level(SENSOR_POWER, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    uint32_t raw = adc1_get_raw(ADC1_CHANNEL_0);
    uint32_t mv  = esp_adc_cal_raw_to_voltage(raw, adc_chars);

    float value = (mv / 1000.0f) * 50.0f;
    value += (int)(esp_random() % 101 - 50) / 100.0f;

    gpio_set_level(SENSOR_POWER, 0);
    return value;
}

static void sensor_timer_callback(TimerHandle_t timer) {
    sensor_data_t s;
    s.value = read_sensor_value();
    s.timestamp = xTaskGetTickCount();
    s.valid = (s.value >= 0 && s.value <= 50);

    health_stats.sensor_readings++;

    if (xQueueSend(sensor_queue, &s, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Sensor queue full");
    }

    /* EXP4: คง adaptive sampling ไว้ เพื่อสะท้อนสุขภาพระบบด้วย */
    TickType_t new_period = (s.value > 40.0f) ? pdMS_TO_TICKS(500)
                        : (s.value > 25.0f) ? pdMS_TO_TICKS(1000)
                                            : pdMS_TO_TICKS(2000);
    xTimerChangePeriod(timer, new_period, 0);
}

/* ================ STATUS / HEALTH REPORT ================ */
static void status_timer_callback(TimerHandle_t timer) {
    health_stats.system_uptime_sec = pdTICKS_TO_MS(xTaskGetTickCount()) / 1000;

    size_t free_heap = esp_get_free_heap_size();
    ESP_LOGI(TAG, "\n════ SYSTEM HEALTH (3s) ════");
    ESP_LOGI(TAG, "Uptime: %lus | Healthy: %s",
             health_stats.system_uptime_sec,
             health_stats.system_healthy ? "✅" : "❌");
    ESP_LOGI(TAG, "Watchdog: feeds=%lu, timeouts=%lu",
             health_stats.watchdog_feeds, health_stats.watchdog_timeouts);
    ESP_LOGI(TAG, "Patterns: changes=%lu, current=%d",
             health_stats.pattern_changes, current_pattern);
    ESP_LOGI(TAG, "Sensor: readings=%lu", health_stats.sensor_readings);
    ESP_LOGI(TAG, "Memory: free_heap=%u bytes", (unsigned)free_heap);
    ESP_LOGI(TAG, "Timers: WD=%s Feed=%s Pat=%s Sen=%s",
             xTimerIsTimerActive(watchdog_timer) ? "ON":"OFF",
             xTimerIsTimerActive(feed_timer)     ? "ON":"OFF",
             xTimerIsTimerActive(pattern_timer)  ? "ON":"OFF",
             xTimerIsTimerActive(sensor_timer)   ? "ON":"OFF");
    ESP_LOGI(TAG, "═══════════════════════════");

    /* สะท้อนสถานะด้วยไฟสถานะ */
    gpio_set_level(STATUS_LED, health_stats.system_healthy ? 1 : 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    gpio_set_level(STATUS_LED, 0);
}

/* ================ TASKS ================ */
static void sensor_processing_task(void *parameter) {
    sensor_data_t s;
    float sum = 0; int cnt = 0;
    while (1) {
        if (xQueueReceive(sensor_queue, &s, portMAX_DELAY) == pdTRUE) {
            if (s.valid) {
                sum += s.value; cnt++;
                if (cnt >= 10) {
                    float avg = sum / cnt;
                    /* ผูก health ด้วยค่าอุณหภูมิระยะยาว */
                    if (avg > 38.0f) {
                        ESP_LOGW(TAG, "🔥 Persistent high temp (avg=%.2f)", avg);
                        health_stats.system_healthy = false;
                        change_led_pattern(PATTERN_FAST_BLINK);
                    } else {
                        health_stats.system_healthy = true;
                    }
                    sum = 0; cnt = 0;
                }
            }
        }
    }
}

static void system_monitor_task(void *parameter) {
    uint32_t last_sensor = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000)); /* ทุก 15 วินาที เช็คเงื่อนไขเชิงระบบ */
        if (health_stats.watchdog_timeouts > 3) {
            ESP_LOGE(TAG, "⚠️ Too many WD timeouts (%lu) -> mark unhealthy",
                     health_stats.watchdog_timeouts);
            health_stats.system_healthy = false;
        }
        if (health_stats.sensor_readings == last_sensor) {
            ESP_LOGW(TAG, "⚠️ Sensor stalled (no new reading in 15s)");
            /* คุณอาจ xTimerReset(sensor_timer,0); ได้ */
        }
        last_sensor = health_stats.sensor_readings;

        size_t free_heap = esp_get_free_heap_size();
        if (free_heap < 12000) {
            ESP_LOGW(TAG, "⚠️ Low heap: %u", (unsigned)free_heap);
        }
    }
}

/* ================ INIT / START ================ */
static void init_hardware(void) {
    gpio_set_direction(STATUS_LED, GPIO_MODE_OUTPUT);
    gpio_set_direction(WATCHDOG_LED, GPIO_MODE_OUTPUT);
    gpio_set_direction(PATTERN_LED_1, GPIO_MODE_OUTPUT);
    gpio_set_direction(PATTERN_LED_2, GPIO_MODE_OUTPUT);
    gpio_set_direction(PATTERN_LED_3, GPIO_MODE_OUTPUT);
    gpio_set_direction(SENSOR_POWER, GPIO_MODE_OUTPUT);

    gpio_set_level(STATUS_LED, 0);
    gpio_set_level(WATCHDOG_LED, 0);
    gpio_set_level(PATTERN_LED_1, 0);
    gpio_set_level(PATTERN_LED_2, 0);
    gpio_set_level(PATTERN_LED_3, 0);
    gpio_set_level(SENSOR_POWER, 0);

    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_11);
    adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, adc_chars);
}

static void create_timers(void) {
    watchdog_timer = xTimerCreate("Watchdog", pdMS_TO_TICKS(WATCHDOG_TIMEOUT_MS), pdFALSE, (void*)1, watchdog_timeout_callback);
    feed_timer     = xTimerCreate("Feed",     pdMS_TO_TICKS(WATCHDOG_FEED_MS),     pdTRUE,  (void*)2, feed_watchdog_callback);
    pattern_timer  = xTimerCreate("Pattern",  pdMS_TO_TICKS(PATTERN_BASE_MS),      pdTRUE,  (void*)3, pattern_timer_callback);
    sensor_timer   = xTimerCreate("Sensor",   pdMS_TO_TICKS(SENSOR_SAMPLE_MS),     pdTRUE,  (void*)4, sensor_timer_callback);
    status_timer   = xTimerCreate("Status",   pdMS_TO_TICKS(STATUS_UPDATE_MS),     pdTRUE,  (void*)5, status_timer_callback);
}

static void create_queues(void) {
    sensor_queue  = xQueueCreate(20, sizeof(sensor_data_t));
    pattern_queue = xQueueCreate(10, sizeof(led_pattern_t));
}

static void start_system(void) {
    xTimerStart(watchdog_timer, 0);
    xTimerStart(feed_timer, 0);
    xTimerStart(pattern_timer, 0);
    xTimerStart(sensor_timer, 0);
    xTimerStart(status_timer, 0);

    xTaskCreate(sensor_processing_task, "SensorProc", 4096, NULL, 6, NULL);
    xTaskCreate(system_monitor_task,   "SysMon",      4096, NULL, 3, NULL);

    change_led_pattern(PATTERN_SLOW_BLINK);
}

void app_main(void) {
    ESP_LOGI(TAG, "EXP4: System Health Monitoring (full)");
    init_hardware();
    create_queues();
    create_timers();
    start_system();
}