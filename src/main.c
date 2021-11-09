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

#include "lwip/err.h"
#include "lwip/sys.h"

#include "owb.h"
#include "owb_rmt.h"
#include "ds18b20.h"

static const char *TAG = "wifi + ds18b20";
#include "secrets.h"
#include "MQTT.h"

#define GPIO_DS18B20_0       (4)
#define MAX_DEVICES          (8)
#define DS18B20_RESOLUTION   (DS18B20_RESOLUTION_12_BIT)
#define SAMPLE_PERIOD        (1000)   // milliseconds

// How many times will try to connect WiFi
#define CONECTION_RETRY  5

static struct timeval app_start;

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

RTC_DATA_ATTR   uint32_t crc32;
RTC_DATA_ATTR   uint8_t channel;
RTC_DATA_ATTR   uint8_t bssid[6];

static int s_retry_num = 0;

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < CONECTION_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
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

    // validate WiFi settings from RTC memory
    bool rtcValid = false;
/*
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

    IP4_ADDR(&ip_info.ip, 192, 168, 100, 210);
   	IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
   	IP4_ADDR(&ip_info.gw, 192, 168, 100, 1);

/*
    ip4addr_aton("192.168.100.210", &ip_info.ip);
    ip4addr_aton("255.255.255.0", &ip_info.netmask);
    ip4addr_aton("192.168.100.1", &ip_info.gw);
*/

/*
    ip4addr_aton("192.168.100.210", &ip_info.ip);
    ip4addr_aton("255.255.255.0", &ip_info.netmask);
    ip4addr_aton("192.168.100.1", &ip_info.gw);
*/
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
                .required = false
            },
        },
    };
	// Need cleaning
/*
	// Need cleaning
    if(rtcValid){
        memcpy(wifi_config.sta.bssid, bssid, sizeof(bssid));
        wifi_config.sta.channel = channel;
		wifi_config.sta.bssid_set = 1;
		wifi_config.sta.scan_method = WIFI_FAST_SCAN;
    }
*/
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

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
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 MY_SECRET_SSID, MY_SECRET_PASS);
/*
        if(!rtcValid){
            memcpy(bssid, wifi_config.sta.bssid, sizeof(wifi_config.sta.bssid));
            channel = wifi_config.sta.channel;
            crc32 = esp_rom_crc32_le( 0, bssid, sizeof( bssid ) );
        }
*/
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 MY_SECRET_SSID, MY_SECRET_PASS);
//        crc32 = 0;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 MY_SECRET_SSID, MY_SECRET_PASS);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

void app_main(void)
{
    gettimeofday (&app_start, NULL);
    printf ("%d.%06d started at %ld.%06ld\n", 0, 0, app_start.tv_sec, app_start.tv_usec );

    float temperature = 0;
    const int wakeup_time_sec = 300;
    printf("Enabling timer wakeup, %ds\n", wakeup_time_sec);
    esp_sleep_enable_timer_wakeup(wakeup_time_sec * 1000000);
    
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");

    // Turn on WiFi
    wifi_init_sta();

    // Create a 1-Wire bus, using the RMT timeslot driver
    OneWireBus * owb;
    owb_rmt_driver_info rmt_driver_info;
    owb = owb_rmt_initialize(&rmt_driver_info, GPIO_DS18B20_0, RMT_CHANNEL_1, RMT_CHANNEL_0);
    owb_use_crc(owb, true);  // enable CRC check for ROM code

    // Find all connected devices
    printf("Find devices:\n");
    OneWireBus_ROMCode device_rom_codes[MAX_DEVICES] = {0};
    int num_devices = 0;
    OneWireBus_SearchState search_state = {0};
    bool found = false;
    owb_search_first(owb, &search_state, &found);
    while (found)
    {
        char rom_code_s[17];
        owb_string_from_rom_code(search_state.rom_code, rom_code_s, sizeof(rom_code_s));
        printf("  %d : %s\n", num_devices, rom_code_s);
        device_rom_codes[num_devices] = search_state.rom_code;
        ++num_devices;
        owb_search_next(owb, &search_state, &found);
    }
    printf("Found %d device%s\n", num_devices, num_devices == 1 ? "" : "s");

    // Create DS18B20 devices on the 1-Wire bus
    DS18B20_Info * devices[MAX_DEVICES] = {0};
    for (int i = 0; i < num_devices; ++i)
    {
        DS18B20_Info * ds18b20_info = ds18b20_malloc();  // heap allocation
        devices[i] = ds18b20_info;

        if (num_devices == 1)
        {
            printf("Single device optimisations enabled\n");
            ds18b20_init_solo(ds18b20_info, owb);          // only one device on bus
        }
        else
        {
            ds18b20_init(ds18b20_info, owb, device_rom_codes[i]); // associate with bus and device
        }
        ds18b20_use_crc(ds18b20_info, true);           // enable CRC check on all reads
        ds18b20_set_resolution(ds18b20_info, DS18B20_RESOLUTION);
    }

    start_MQTT();

    // Read temperatures more efficiently by starting conversions on all devices at the same time
    int errors_count[MAX_DEVICES] = {0};
    int sample_count = 0;
    if (num_devices > 0)
    {
        for ( int tmp = 0; tmp < 1; ++tmp)
        {
            ds18b20_convert_all(owb);

            // In this application all devices use the same resolution,
            // so use the first device to determine the delay
            ds18b20_wait_for_conversion(devices[0]);

            // Read the results immediately after conversion otherwise it may fail
            // (using printf before reading may take too long)
            float readings[MAX_DEVICES] = { 0 };
            DS18B20_ERROR errors[MAX_DEVICES] = { 0 };

            for (int i = 0; i < num_devices; ++i)
            {
                errors[i] = ds18b20_read_temp(devices[i], &readings[i]);
            }

            // Print results in a separate loop, after all have been read
            printf("\nTemperature readings (degrees C): sample %d\n", ++sample_count);
            for (int i = 0; i < num_devices; ++i)
            {
                if (errors[i] != DS18B20_OK)
                {
                    ++errors_count[i];
                }

                printf("  %d: %.1f    %d errors\n", i, readings[i], errors_count[i]);
            }
            temperature = readings[0];

//            vTaskDelayUntil(&last_wake_time, SAMPLE_PERIOD / portTICK_PERIOD_MS);
        }
    }
    else
    {
        printf("\nNo DS18B20 devices detected!\n");
    }

    // clean up dynamically allocated data
    for (int i = 0; i < num_devices; ++i)
    {
        ds18b20_free(&devices[i]);
    }
    owb_uninitialize(owb);

    //Send MQTT
    char message[7];
    char topic[50];
    sprintf(message, "%.1f", temperature);
    sprintf(topic, "Temperature-monitors/Testing");
    send_MQTT(topic, message);
    
    end_MQTT();

    printf("Turning off WiFi\n");
    
    ESP_ERROR_CHECK(esp_wifi_stop());

    struct timeval tv;

    gettimeofday (&tv, NULL);
    tv.tv_sec  -= app_start.tv_sec;
    tv.tv_usec -= app_start.tv_usec;
    if (tv.tv_usec < 0) {
	    tv.tv_usec += 1000000;
	    --tv.tv_sec;
    }
	
    printf("Entering deep sleep after %ld.%06ld seconds\n", tv.tv_sec, tv.tv_usec);

    // Allow prints to finish
    fflush(stdout);

    esp_deep_sleep_start();
}
