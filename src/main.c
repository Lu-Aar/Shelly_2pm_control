#include <string.h>
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_sleep.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/etharp.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "driver/gpio.h"
#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"
#include "ShutterControl.h"
#include "config.h"

// #define DEBUG
struct netif *esp_netif_get_netif_impl(esp_netif_t *esp_netif);

#define ESP_MAXIMUM_RETRY 10

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK

#define START_IP 1
#define END_IP 254

#define BUTTON_UP GPIO_NUM_5
#define BUTTON_STOP GPIO_NUM_4
#define BUTTON_DOWN GPIO_NUM_3

#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"

#define AWAKE_TIME_MS 20000

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

const uint8_t SHUTTER_MAC_1[] = {0xE4, 0xB3, 0x23, 0x23, 0x4B, 0x1C};
const uint8_t SHUTTER_MAC_2[] = {0xE4, 0xB3, 0x23, 0x1F, 0xFA, 0xAC};

void init(void);

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
#ifdef DEBUG
            ESP_LOGI("WIFI", "retry to connect to the AP");
#endif
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
#ifdef DEBUG
        ESP_LOGI("WIFI", "connect to the AP fail");
#endif
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
#ifdef DEBUG
        ESP_LOGI("WIFI", "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
#endif
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void DisableUnusedPins(void)
{
    gpio_config_t io_conf_0 = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << GPIO_NUM_0),
    };
    gpio_config(&io_conf_0);
    gpio_set_level(GPIO_NUM_0, 0);

    gpio_config_t io_conf_rest = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << GPIO_NUM_1) |
                        (1ULL << GPIO_NUM_2) |
                        (1ULL << GPIO_NUM_6) |
                        (1ULL << GPIO_NUM_7) |
                        (1ULL << GPIO_NUM_8) |
                        (1ULL << GPIO_NUM_9) |
                        (1ULL << GPIO_NUM_10) |
                        (1ULL << GPIO_NUM_20) |
                        (1ULL << GPIO_NUM_21),
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io_conf_rest);
}

ip4_addr_t RequestIp(const uint8_t *shutter_mac)
{
    ip4_addr_t result_ip;
    IP4_ADDR(&result_ip, 255, 255, 255, 255);

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(netif, &ip_info);

    struct netif *lwip_netif = esp_netif_get_netif_impl(netif);

    for (int ii = START_IP; ii <= END_IP; ++ii)
    {
        ip4_addr_t target_ip;
        IP4_ADDR(&target_ip,
                 ip_info.ip.addr & 0xFF,
                 (ip_info.ip.addr >> 8) & 0xFF,
                 (ip_info.ip.addr >> 16) & 0xFF,
                 ii);

        if (etharp_request(lwip_netif, &target_ip) != ERR_OK)
        {
#ifdef DEBUG
            ESP_LOGI("SHUTTER", "etharp_request failed for IP: " IPSTR, IP2STR(&target_ip));
#endif
        }
        vTaskDelay(pdMS_TO_TICKS(50));

        if ((ii % 10 == 0) || (ii == END_IP - 1))
        {
            struct eth_addr *mac;
            struct netif *netif_out;
            ip4_addr_t *ip_out;

            for (int jj = 0; jj < 10; ++jj)
            {
                if (etharp_get_entry(jj, &ip_out, &netif_out, &mac))
                {
                    if (memcmp(mac->addr, shutter_mac, 6) == 0)
                    {
#ifdef DEBUG
                        ESP_LOGI("SHUTTER", "Found MAC: " MACSTR " for IP: " IPSTR,
                                 mac->addr[0], mac->addr[1], mac->addr[2], mac->addr[3], mac->addr[4], mac->addr[5], IP2STR(ip_out));
#endif
                        return *ip_out;
                        break;
                    }
                }
            }
        }
    }
    return result_ip;
}

void ESPSleep(void)
{
#ifdef DEBUG
    ESP_LOGI("SLEEP", "Going to sleep now");
#endif
    DisableUnusedPins();
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_deep_sleep_start();
}

void setup_wakeup()
{
    const uint64_t wakeup_pins = 1 << BUTTON_UP | 1 << BUTTON_STOP | 1 << BUTTON_DOWN;
    esp_deep_sleep_enable_gpio_wakeup(wakeup_pins, ESP_GPIO_WAKEUP_GPIO_LOW);
}

void GetIps(ip4_addr_t *pShutter1_ip, ip4_addr_t *pShutter2_ip)
{
    nvs_handle_t handle;
    nvs_open("storage", NVS_READWRITE, &handle);
    esp_err_t err1 = nvs_get_u32(handle, "ip1", &pShutter1_ip->addr);
    esp_err_t err2 = nvs_get_u32(handle, "ip2", &pShutter2_ip->addr);
    if (err1 == ESP_ERR_NVS_NOT_FOUND)
    {
        *pShutter1_ip = RequestIp(SHUTTER_MAC_1);
        nvs_set_u32(handle, "ip1", pShutter1_ip->addr);
#ifdef DEBUG
        ESP_LOGI("NVM", "Saved shutter1 IP");
#endif
    }
    if (err2 == ESP_ERR_NVS_NOT_FOUND)
    {
        *pShutter2_ip = RequestIp(SHUTTER_MAC_2);
        nvs_set_u32(handle, "ip2", pShutter2_ip->addr);
#ifdef DEBUG
        ESP_LOGI("NVM", "Saved shutter2 IP");
#endif
    }
    if (err1 == ESP_ERR_NVS_NOT_FOUND || err2 == ESP_ERR_NVS_NOT_FOUND)
    {
        nvs_commit(handle);
    }
    nvs_close(handle);

#ifdef DEBUG
    ESP_LOGI("SHUTTER", "Shutter1 IP: " IPSTR, IP2STR(&shutter1_ip));
    ESP_LOGI("SHUTTER", "Shutter2 IP: " IPSTR, IP2STR(&shutter2_ip));
#endif
}

void SetIps(ip4_addr_t shutter1_ip, ip4_addr_t shutter2_ip)
{
    nvs_handle_t handle;
    nvs_open("storage", NVS_READWRITE, &handle);
    nvs_set_u32(handle, "ip1", shutter1_ip.addr);
    nvs_set_u32(handle, "ip2", shutter2_ip.addr);
    nvs_commit(handle);
    nvs_close(handle);

#ifdef DEBUG
    ESP_LOGI("SHUTTER", "Shutter1 IP: " IPSTR, IP2STR(&shutter1_ip));
    ESP_LOGI("SHUTTER", "Shutter2 IP: " IPSTR, IP2STR(&shutter2_ip));
#endif
}

void Open(ip4_addr_t ip1, ip4_addr_t ip2)
{
    OpenShutter(ip1);
    OpenShutter(ip2);
}

void Stop(ip4_addr_t ip1, ip4_addr_t ip2)
{
    enum ShutterStatus state1 = GetShutterStatus(ip1);
    enum ShutterStatus state2 = GetShutterStatus(ip2);
#ifdef DEBUG
    ESP_LOGI("STATUS", "The shutter status is: %d", state1);
    ESP_LOGI("STATUS", "The shutter status is: %d", state2);
#endif
    if (state1 == SHUTTER_STATUS_OPENING || state1 == SHUTTER_STATUS_CLOSING)
    {
        StopShutter(ip1);
    }
    else if (state1 == SHUTTER_STATUS_STOPPED || state1 == SHUTTER_STATUS_OPEN || state1 == SHUTTER_STATUS_CLOSED)
    {
        SetShutterPosition(ip1, SHUTTER_VENTILATION);
    }

    if (state2 == SHUTTER_STATUS_OPENING || state2 == SHUTTER_STATUS_CLOSING)
    {
        StopShutter(ip2);
    }
    else if (state2 == SHUTTER_STATUS_STOPPED || state2 == SHUTTER_STATUS_OPEN || state2 == SHUTTER_STATUS_CLOSED)
    {
        SetShutterPosition(ip2, SHUTTER_VENTILATION);
    }
}

void Close(ip4_addr_t ip1, ip4_addr_t ip2)
{
    CloseShutter(ip1);
    CloseShutter(ip2);
}

void app_main()
{
    int gpioStates = REG_READ(GPIO_IN_REG);
    int buttonUpState = !((gpioStates >> BUTTON_UP) & 1);
    int buttonStopState = !((gpioStates >> BUTTON_STOP) & 1);
    int buttonDownState = !((gpioStates >> BUTTON_DOWN) & 1);
    init();
#ifdef DEBUG
    ESP_LOGI("BUTTON", "Button UP state: %d", buttonUpState);
    ESP_LOGI("BUTTON", "Button STOP state: %d", buttonStopState);
    ESP_LOGI("BUTTON", "Button DOWN state: %d", buttonDownState);
#endif

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BUTTON_UP) | (1ULL << BUTTON_STOP) | (1ULL << BUTTON_DOWN),
    };
    gpio_config(&io_conf);
    setup_wakeup();

    ip4_addr_t shutter1_ip;
    ip4_addr_t shutter2_ip;
    GetIps(&shutter1_ip, &shutter2_ip);

    const TickType_t awake_time = pdMS_TO_TICKS(AWAKE_TIME_MS);
    TickType_t last_wake = xTaskGetTickCount();

    while (1)
    {
        if (!buttonUpState && !buttonStopState && !buttonDownState)
        {
            gpioStates = REG_READ(GPIO_IN_REG);
            buttonUpState = !((gpioStates >> BUTTON_UP) & 1);
            buttonStopState = !((gpioStates >> BUTTON_STOP) & 1);
            buttonDownState = !((gpioStates >> BUTTON_DOWN) & 1);
        }
        if (buttonUpState || buttonStopState || buttonDownState)
        {
            last_wake = xTaskGetTickCount();
        }

        if ((xTaskGetTickCount() - last_wake) > awake_time)
        {
            ESPSleep();
        }

        if (buttonUpState && buttonDownState)
        {
            shutter1_ip = RequestIp(SHUTTER_MAC_1);
            shutter2_ip = RequestIp(SHUTTER_MAC_2);
            SetIps(shutter1_ip, shutter2_ip);

            buttonUpState = 0;
            buttonDownState = 0;
        }
        else
        {
            if (buttonUpState)
            {
                Open(shutter1_ip, shutter2_ip);
                buttonUpState = 0;
            }
            if (buttonStopState)
            {
                Stop(shutter1_ip, shutter2_ip);
                buttonStopState = 0;
            }
            if (buttonDownState)
            {
                Close(shutter1_ip, shutter2_ip);
                buttonDownState = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS},
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

#ifdef DEBUG
    ESP_LOGI("WIFI", "wifi_init_sta finished.");
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI("WIFI", "connected to ap SSID:%s",
                 WIFI_SSID);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI("WIFI", "Failed to connect to SSID:%s",
                 WIFI_SSID);
    }
    else
    {
        ESP_LOGE("WIFI", "UNEXPECTED EVENT");
    }
#endif
}

void init()
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

#ifdef DEBUG
    if (CONFIG_LOG_MAXIMUM_LEVEL > CONFIG_LOG_DEFAULT_LEVEL)
    {
        esp_log_level_set("wifi", CONFIG_LOG_MAXIMUM_LEVEL);
    }
    ESP_LOGI("WIFI", "ESP_WIFI_MODE_STA");
#endif
    wifi_init_sta();
}