#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#define UART_PORT_NUM UART_NUM_0
#define UART_BAUD_RATE 115200
#define BUFFER_SIZE 1024

#define EXAMPLE_MAX_CHAR_SIZE 32
uint8_t archive_name[EXAMPLE_MAX_CHAR_SIZE];
uint8_t archive_name_index = 0;

static const char *TAG = "sd_card";
#define MOUNT_POINT "/sdcard"
#define PIN_NUM_MOSI 23
#define PIN_NUM_MISO 19
#define PIN_NUM_CLK 18
#define PIN_NUM_CS 5

EventGroupHandle_t xEventGroup;
const int FILE_NAME_READY_BIT = BIT0; // Bit para indicar que el nombre del archivo está completo

static esp_err_t read_file(const char *filename, char *message_compressed)
{
    FILE *f = fopen(filename, "r");
    if (f == NULL)
    {
        return ESP_FAIL;
    }
    fgets(message_compressed, EXAMPLE_MAX_CHAR_SIZE, f);
    fclose(f);
    return ESP_OK;
}

static void decompress_message(char *compressed, char *decompressed)
{
    uint8_t index, repeat;
    uint8_t compressed_index = 0, decompressed_index = 0;

    while (compressed[compressed_index] != '\0')
    {
        if (compressed[compressed_index] == '[')
        {
            compressed_index++;
            repeat = 0;
            while (compressed[compressed_index] >= '0' && compressed[compressed_index] <= '9')
            {
                repeat = repeat * 10 + (compressed[compressed_index] - '0');
                compressed_index++;
            }
            if (compressed[compressed_index] == ']')
            {
                compressed_index++;
                for (index = 0; index < repeat; index++)
                {
                    decompressed[decompressed_index] = compressed[compressed_index];
                    decompressed_index++;
                }
                compressed_index++;
            }
            else
            {
                uint8_t block_start_index = compressed_index;
                uint8_t bracket_counter = 1;

                while (bracket_counter > 0)
                {
                    if (compressed[compressed_index] == '[')
                        bracket_counter++;
                    if (compressed[compressed_index] == ']')
                        bracket_counter--;

                    compressed_index++;
                }

                uint8_t block_size = compressed_index - block_start_index - 1;
                char block[block_size + 1];
                strncpy(block, compressed + block_start_index, block_size);
                block[block_size] = '\0';

                char decompressed_block[EXAMPLE_MAX_CHAR_SIZE];
                decompress_message(block, decompressed_block);

                for (index = 0; index < repeat; index++)
                {
                    strcpy(decompressed + decompressed_index, decompressed_block);
                    decompressed_index += strlen(decompressed_block);
                }
            }
        }
        else
        {
            decompressed[decompressed_index++] = compressed[compressed_index++];
        }
    }
    decompressed[decompressed_index] = '\0';
}

static void get_archive_name(void *params)
{
    const uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE};

    uart_driver_install(UART_PORT_NUM, BUFFER_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_PORT_NUM, &uart_config);
    uart_set_pin(UART_PORT_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // Buffer temporal para los datos entrantes
    uint8_t *data = (uint8_t *)malloc(BUFFER_SIZE);
    archive_name[EXAMPLE_MAX_CHAR_SIZE - 1] = '\0';

    while (1)
    {
        int len = uart_read_bytes(UART_PORT_NUM, data, (BUFFER_SIZE - 1), 20 / portTICK_PERIOD_MS);

        for (int len_counter = 0; len_counter < len; len_counter++)
        {
            uint8_t current_char = data[len_counter];
            switch (current_char)
            {
            case 8: // Backspace
                if (archive_name_index > 0)
                {
                    archive_name_index -= 1;
                }
                uart_write_bytes(UART_PORT_NUM, (const char *)"\b \b", 3);
                archive_name[archive_name_index] = ' ';
                break;
            case 13:                                                  // Enter
                xEventGroupSetBits(xEventGroup, FILE_NAME_READY_BIT); // Nombre del archivo completo
                break;
            default: // Caracteres validos
                if (((current_char >= 'a' && current_char <= 'z') ||
                     (current_char >= 'A' && current_char <= 'Z') ||
                     (current_char >= '0' && current_char <= '9') ||
                     (current_char == '.' || current_char == '_' || current_char == '-')) &&
                    (archive_name_index < EXAMPLE_MAX_CHAR_SIZE - 1))
                {
                    archive_name[archive_name_index] = current_char;
                    archive_name_index++;
                    char temp[2] = {current_char, '\0'};
                    uart_write_bytes(UART_PORT_NUM, (const char *)temp, 1);
                }
                break;
            }
            vTaskDelay(20 / portTICK_PERIOD_MS);
        }
    }
}

static void sd_task(void *params)
{
    char fullpath[EXAMPLE_MAX_CHAR_SIZE * 2];
    char compressed_message[EXAMPLE_MAX_CHAR_SIZE];
    char decompressed_message[EXAMPLE_MAX_CHAR_SIZE];

    uart_write_bytes(UART_PORT_NUM, (const char *)"\033[H\033[J", 6);

    while (1)
    {
        uart_write_bytes(UART_PORT_NUM, "\n\nNombre del archivo: ", strlen("\n\nNombre del archivo: "));

        xEventGroupWaitBits(xEventGroup, FILE_NAME_READY_BIT, pdTRUE, pdFALSE, portMAX_DELAY);

        snprintf(fullpath, sizeof(fullpath), "%s/%s", MOUNT_POINT, archive_name);

        if (read_file(fullpath, compressed_message) == ESP_OK)
        {
            uart_write_bytes(UART_PORT_NUM, "\nMensaje: ", strlen("\nMensaje: "));
            decompress_message(compressed_message, decompressed_message);
            uart_write_bytes(UART_PORT_NUM, decompressed_message, strlen(decompressed_message));
        }
        else
        {
            uart_write_bytes(UART_PORT_NUM, "\nArchivo no encontrado.", strlen("\nArchivo no encontrado."));
        }

        archive_name_index = 0;                        // Reiniciar el nombre del archivo después de procesarlo
        memset(archive_name, 0, sizeof(archive_name)); // Limpiar el nombre del archivo

        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
}

void app_main(void)
{
    xEventGroup = xEventGroupCreate();

    // Inicializar SD
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024};

    sdmmc_card_t *card;
    const char mount_point[] = MOUNT_POINT;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 5000;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Fallo al inicializar el bus SPI.");
        return;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "No se pudo montar el sistema de archivos.");
        return;
    }

    xTaskCreate(get_archive_name, "get_archive_name", 2048, NULL, 1, NULL);
    xTaskCreate(sd_task, "sd_task", 4096, NULL, 1, NULL);
}
