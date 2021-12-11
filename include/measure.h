//measure.h

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "owb.h"
#include "owb_rmt.h"
#include "ds18b20.h"

#define GPIO_DS18B20_0 (4)
#define MAX_DEVICES (8)
#define DS18B20_RESOLUTION (DS18B20_RESOLUTION_12_BIT)
#define SAMPLE_PERIOD (1000) // milliseconds

float temperature = 0;
bool ready_flag = false;

void vTaskMeasure(void *param)
{

    for (;;)
    {
        // Create a 1-Wire bus, using the RMT timeslot driver
        OneWireBus *owb;
        owb_rmt_driver_info rmt_driver_info;
        owb = owb_rmt_initialize(&rmt_driver_info, GPIO_DS18B20_0, RMT_CHANNEL_1, RMT_CHANNEL_0);
        owb_use_crc(owb, true); // enable CRC check for ROM code

        // Find all connected devices
        ESP_LOGD(TAG, "Find devices:\n");
        OneWireBus_ROMCode device_rom_codes[MAX_DEVICES] = {0};
        int num_devices = 0;
        OneWireBus_SearchState search_state = {0};
        bool found = false;
        owb_search_first(owb, &search_state, &found);
        while (found)
        {
            char rom_code_s[17];
            owb_string_from_rom_code(search_state.rom_code, rom_code_s, sizeof(rom_code_s));
            ESP_LOGD(TAG, "  %d : %s\n", num_devices, rom_code_s);
            device_rom_codes[num_devices] = search_state.rom_code;
            ++num_devices;
            owb_search_next(owb, &search_state, &found);
        }
        ESP_LOGI(TAG, "Found %d device%s\n", num_devices, num_devices == 1 ? "" : "s");

        // Create DS18B20 devices on the 1-Wire bus
        DS18B20_Info *devices[MAX_DEVICES] = {0};
        for (int i = 0; i < num_devices; ++i)
        {
            DS18B20_Info *ds18b20_info = ds18b20_malloc(); // heap allocation
            devices[i] = ds18b20_info;

            if (num_devices == 1)
            {
                ESP_LOGI(TAG, "Single device optimisations enabled\n");
                ds18b20_init_solo(ds18b20_info, owb); // only one device on bus
            }
            else
            {
                ds18b20_init(ds18b20_info, owb, device_rom_codes[i]); // associate with bus and device
            }
            ds18b20_use_crc(ds18b20_info, true); // enable CRC check on all reads
            ds18b20_set_resolution(ds18b20_info, DS18B20_RESOLUTION);
        }

        // Read temperatures more efficiently by starting conversions on all devices at the same time
        int errors_count[MAX_DEVICES] = {0};
        int sample_count = 0;
        if (num_devices > 0)
        {
            for (int tmp = 0; tmp < 1; ++tmp)
            {
                ds18b20_convert_all(owb);

                // In this application all devices use the same resolution,
                // so use the first device to determine the delay
                ds18b20_wait_for_conversion(devices[0]);

                // Read the results immediately after conversion otherwise it may fail
                // (using printf before reading may take too long)
                float readings[MAX_DEVICES] = {0};
                DS18B20_ERROR errors[MAX_DEVICES] = {0};

                for (int i = 0; i < num_devices; ++i)
                {
                    errors[i] = ds18b20_read_temp(devices[i], &readings[i]);
                }

                // Print results in a separate loop, after all have been read
                ESP_LOGW(TAG, "Temperature readings (degrees C): sample %d", ++sample_count);
                for (int i = 0; i < num_devices; ++i)
                {
                    if (errors[i] != DS18B20_OK)
                    {
                        ++errors_count[i];
                    }

                    ESP_LOGW(TAG, "  %d: %.1f    %d errors", i, readings[i], errors_count[i]);
                }
                temperature = readings[0];
                xSemaphoreGive(meas_done);

                // vTaskDelayUntil(&last_wake_time, SAMPLE_PERIOD / portTICK_PERIOD_MS);
            }
        }
        else
        {
            ESP_LOGE(TAG, "\nNo DS18B20 devices detected!\n");
        }

        // clean up dynamically allocated data
        for (int i = 0; i < num_devices; ++i)
        {
            ds18b20_free(&devices[i]);
        }
        owb_uninitialize(owb);
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }

    vTaskDelete(NULL);
}