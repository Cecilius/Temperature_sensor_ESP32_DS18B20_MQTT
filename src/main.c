// main.c

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_sleep.h"
#include "esp_rom_crc.h"
#include <driver/adc.h>

#include "lwip/err.h"
#include "lwip/sys.h"

static const char *TAG = "wifi + ds18b20";
#include "secrets.h"
#include "ha_config.h"
#include "MQTT.h"
#include "measure.h"

// How many times will try to connect WiFi
#define CONECTION_RETRY 5

static struct timeval app_start;

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

/*
RTC_DATA_ATTR uint32_t crc32;
RTC_DATA_ATTR uint8_t channel;
RTC_DATA_ATTR uint8_t bssid[6];
*/

static int s_retry_num = 0;

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < CONECTION_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    // WiFi Persistent
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    /*
    // validate WiFi settings from RTC memory
    bool rtcValid = false;
    // Calculate the CRC of bssid from RTC memory
    uint32_t crc = esp_rom_crc32_le( 0, ((uint8_t*)&bssid), sizeof( bssid ) );
    if( crc == crc32 ) {
        rtcValid = true;
        printf("RTC data are valid, using them to fast connect. BSSID-%x:%x:%x:%x:%x:%x, channel: %d\n", bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], channel);
    }
*/
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *my_sta = esp_netif_create_default_wifi_sta();

    esp_netif_dhcpc_stop(my_sta);
    esp_netif_ip_info_t ip_info;

    esp_netif_str_to_ip4(MY_SECRET_STATIC_IP, &ip_info.ip);
    esp_netif_str_to_ip4(MY_SECRET_STATIC_MASK, &ip_info.netmask);
    esp_netif_str_to_ip4(MY_SECRET_STATIC_GW, &ip_info.gw);

    esp_netif_set_ip_info(my_sta, &ip_info);

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
            .ssid = MY_SECRET_SSID,
            .password = MY_SECRET_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
            * However these modes are deprecated and not advisable to be used. Incase your Access point
            * doesn't support WPA2, these mode can be enabled by commenting below line */
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false},
        },
    };
    // Need cleaning
    /*
    if(rtcValid){
        memcpy(wifi_config.sta.bssid, bssid, sizeof(bssid));
        wifi_config.sta.channel = channel;
		wifi_config.sta.bssid_set = 1;
		wifi_config.sta.scan_method = WIFI_FAST_SCAN;
    }
*/
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 MY_SECRET_SSID, MY_SECRET_PASS);
        /*
        if(!rtcValid){
            memcpy(bssid, wifi_config.sta.bssid, sizeof(wifi_config.sta.bssid));
            channel = wifi_config.sta.channel;
            crc32 = esp_rom_crc32_le( 0, bssid, sizeof( bssid ) );
        }
*/
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 MY_SECRET_SSID, MY_SECRET_PASS);
        // crc32 = 0;
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

void app_main(void)
{
    gettimeofday(&app_start, NULL);
    printf("%d.%06d started at %ld.%06ld\n", 0, 0, app_start.tv_sec, app_start.tv_usec);

    const int wakeup_time_sec = 300;
    printf("Enabling timer wakeup, %ds\n", wakeup_time_sec);
    esp_sleep_enable_timer_wakeup(wakeup_time_sec * 1000000);

    //configure ADC and get reading from battery
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_0);
    // Reading divided by full range times referent voltage * voltage divider
    int raw_adc = adc1_get_raw(ADC1_CHANNEL_0);
    printf("Raw ADC = %d\n", raw_adc);
    float battery = (float)raw_adc / 4096 * 1.1 / 22 * (22 + 68);

    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");

    // Create task to measure
    xTaskCreate(vTaskMeasure, "DS18B20", 1024 * 4, NULL, 3, NULL);

    // Turn on WiFi
    wifi_init_sta();

    start_MQTT();

    //Send MQTT
    char message[7];
    char topic[50];
    sprintf(message, "%.2f", battery);
    sprintf(topic, "%s/%s/battery/state", HA_UNIQ_ID, HA_COMPONENT);
    send_MQTT(topic, message);
    sprintf(message, "%.1f", temperature);
    sprintf(topic, "%s/%s/%s/state", HA_UNIQ_ID, HA_COMPONENT, HA_SENSOR);
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP)
    {
        char conf_topic[80];
        char conf_message[400];
        sprintf(conf_topic, "homeassistant/%s/%s/%s/config", HA_COMPONENT, HA_UNIQ_ID, HA_SENSOR);
        sprintf(conf_message, "{\"dev_cla\":\"%s\",\"unit_of_meas\":\"%s\",\"stat_cla\":\"%s\",\"name\":\"%s\",\"stat_t\":\"%s\",\"uniq_id\":\"%s-%s\",\"dev\":{\"ids\":\"%s\",\"name\":\"%s\",\"sw\":\"%s\"}}", HA_SENSOR, HA_UNIT_OF_MEAS, HA_STAT_CLA, HA_NAME, topic, HA_UNIQ_ID, HA_SENSOR, HA_DEV_ID, HA_DEV_NAME, HA_DEV_SW);
        send_MQTT(conf_topic, conf_message);
        sprintf(conf_topic, "homeassistant/%s/%s/battery/config", HA_COMPONENT, HA_UNIQ_ID);
        sprintf(conf_message, "{\"dev_cla\":\"voltage\",\"unit_of_meas\":\"V\",\"stat_cla\":\"measurement\",\"name\":\"Battery level\",\"stat_t\":\"%s/%s/battery/state\",\"uniq_id\":\"%s-battery\",\"dev\":{\"ids\":\"%s\",\"name\":\"%s\",\"sw\":\"%s\"}}", HA_UNIQ_ID, HA_COMPONENT, HA_UNIQ_ID, HA_DEV_ID, HA_DEV_NAME, HA_DEV_SW);
        send_MQTT(conf_topic, conf_message);
    }

    // wait till we have result of measurement (In case it'll be too slow)
    int wait = 0;
    while (!ready_flag && wait < 20)
    {
        vTaskDelay(100 / portTICK_PERIOD_MS);
        wait++;
    }
    printf("Waited %d00 ms to get measurement.", wait);

    send_MQTT(topic, message);

    end_MQTT();

    printf("Turning off WiFi\n");

    ESP_ERROR_CHECK(esp_wifi_stop());

    struct timeval tv;

    gettimeofday(&tv, NULL);
    tv.tv_sec -= app_start.tv_sec;
    tv.tv_usec -= app_start.tv_usec;
    if (tv.tv_usec < 0)
    {
        tv.tv_usec += 1000000;
        --tv.tv_sec;
    }

    printf("Entering deep sleep after %ld.%06ld seconds\n", tv.tv_sec, tv.tv_usec);

    // Allow prints to finish
    fflush(stdout);

    esp_deep_sleep_start();
}
