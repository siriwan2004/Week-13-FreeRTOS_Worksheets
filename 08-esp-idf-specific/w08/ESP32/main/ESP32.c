#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"

static const char *TAG = "EX3";
#define LED_GPIO GPIO_NUM_2
static SemaphoreHandle_t timer_sema;

/////////////////////////////////////////////////////
// Timer callback (GPTIMER) -> give semaphore from ISR
bool IRAM_ATTR timer_callback(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user)
{
    BaseType_t awoken = pdFALSE;
    SemaphoreHandle_t sem = (SemaphoreHandle_t)user;
    xSemaphoreGiveFromISR(sem, &awoken);
    return (awoken == pdTRUE);
}

void hardware_task(void *arg)
{
    bool led = false;
    while (1) {
        if (xSemaphoreTake(timer_sema, portMAX_DELAY) == pdTRUE) {
            led = !led;
            gpio_set_level(LED_GPIO, led);
            ESP_LOGI(TAG, "Timer tick toggled LED=%d on core %d", led, xPortGetCoreID());
        }
    }
}

/////////////////////////////////////////////////////
// GPIO ISR (button)
static void IRAM_ATTR gpio_isr_handler(void* arg) {
    // Minimal ISR: could set event/sem here
}

void gpio_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
}

/////////////////////////////////////////////////////
// WiFi init (skeleton, uses event loop)
static EventGroupHandle_t s_wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Retrying WiFi");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Got IP");
    }
}

void wifi_init_task(void *arg)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = "YourSSID",
            .password = "YourPassword",
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait until connected, then delete task
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi connected; wifi_init_task done");
    vTaskDelete(NULL);
}

/////////////////////////////////////////////////////
// SPI/I2C worker stubs (implement per hardware)
void bus_worker_task(void *arg)
{
    while (1) {
        // ทำงาน SPI / I2C จริงๆ ที่นี่
        ESP_LOGI(TAG, "SPI/I2C worker on core %d", xPortGetCoreID());
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/////////////////////////////////////////////////////
void app_main(void)
{
    ESP_LOGI(TAG, "Exercise 3 - Peripheral Integration");

    gpio_init();

    // Create semaphore for timer events
    timer_sema = xSemaphoreCreateBinary();

    // Setup GPTIMER: 1 Hz example (ปรับตามต้องการ)
    gptimer_handle_t gptimer = NULL;
    gptimer_config_t config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000  // 1 MHz
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&config, &gptimer));
    gptimer_alarm_config_t alarm = {
        .reload_count = 0,
        .alarm_count = 1000000, // 1s
        .flags.auto_reload_on_alarm = true
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &alarm));
    gptimer_event_callbacks_t cbs = {.on_alarm = timer_callback};
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer, &cbs, timer_sema));
    ESP_ERROR_CHECK(gptimer_enable(gptimer));
    ESP_ERROR_CHECK(gptimer_start(gptimer));

    // Create hardware handler pinned to core 0 for real-time reaction
    xTaskCreatePinnedToCore(hardware_task, "HWTask", 3072, NULL, 15, NULL, 0);

    // WiFi init task (pin to core 1)
    xTaskCreatePinnedToCore(wifi_init_task, "WiFiInit", 4096, NULL, 10, NULL, 1);

    // SPI/I2C worker (on core 1)
    xTaskCreatePinnedToCore(bus_worker_task, "BusWorker", 4096, NULL, 8, NULL, 1);
}
