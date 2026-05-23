/*
 * ESP32-S3 Zarah Voice Assistant V3.0 - Zarah Kasuwanci Edition
 * --------------------------------
 * Standalone Voice Assistant using ESP32-S3.
 * Uses INMP441 for I2S Microphone (VAD & Recording).
 * Uses MAX98357A for I2S Speaker (Playback).
 * Uses 1.8" SPI TFT LCD (ST7735, 128x160) for visual feedback.
 * Uses FATFS SDMMC (1-bit mode) for unlimited JSON-based customer transaction storage.
 * Calls Gemini API for STT and LLM, and Gemini TTS.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_tls.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_idf_version.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include <esp_http_server.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2s.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "esp_task_wdt.h"
#include "cJSON.h"
#include "System5x7.h"

static const char *TAG = "zarah_v3";

// ─── CONFIGURATION ────────────────────────────────────────────────────────────
// API Keys
#define GEMINI_API_KEY       "AIzaSyBFWc3zp3wK_sfPnnehCiC1bdKalnrNDP8"
#define GEMINI_HOST      "generativelanguage.googleapis.com"
#define GEMINI_PORT      443

#define GEMINI_MODEL     "gemini-3.1-flash-lite-preview"
#define TTS_MODEL        "gemini-2.5-flash-preview-tts"

// Pin Map (ESP32-S3 WROOM Camera Board header alignment - CAMERA FRIENDLY PORT)
#define VIBRATION_SENSOR_PIN GPIO_NUM_2
#define LED_PIN              GPIO_NUM_48
// DEVICE_PIN: DISABLED — GPIO 33 is used by Octal PSRAM (SPIIO4) on ESP32-S3-WROOM and MUST NOT be touched.
// If a device control pin is needed, use a free GPIO like GPIO_NUM_4 or GPIO_NUM_5.
#define DEVICE_PIN           (-1)
#define BUTTON_PIN           GPIO_NUM_0  // BOOT button
#define MODE_SWITCH_PIN      GPIO_NUM_1  // Slide switch

// I2S Mic Pins (INMP441)
#define I2S_MIC_BCLK GPIO_NUM_4
#define I2S_MIC_WS   GPIO_NUM_5
#define I2S_MIC_DATA GPIO_NUM_6

// I2S Speaker Pins (MAX98357A)
#define I2S_SPK_BCLK GPIO_NUM_15
#define I2S_SPK_WS   GPIO_NUM_16
#define I2S_SPK_DATA GPIO_NUM_17

// 1.8" SPI TFT LCD Pins (ST7735) - Non-overlapping with Camera
#define LCD_HOST      SPI2_HOST
#define LCD_GPIO_CLK  45                 // Strapping pin (safe output after boot)
#define LCD_GPIO_MOSI 46                 // Strapping pin (safe output after boot)
#define LCD_GPIO_CS   21                 // SD D3 (has pullup, safe for CS)
#define LCD_GPIO_DC   47                 // Completely free pin
#define LCD_GPIO_RST  -1                 // Connected to physical EN/RST pin of the board (no GPIO pin needed)

#define LCD_WIDTH     160
#define LCD_HEIGHT    128

// LCD 16-bit 565 Colors (Big-endian swapped in driver)
#define RGB_BLACK    0x0000
#define RGB_WHITE    0xFFFF
#define RGB_RED      0xF800
#define RGB_GREEN    0x07E0
#define RGB_BLUE     0x001F
#define RGB_YELLOW   0xFFE0
#define RGB_MAGENTA  0xF81F
#define RGB_CYAN     0x07FF
#define RGB_ORANGE   0xFD20
#define RGB_DARKGRAY 0x18E3

// Audio Config
#define MIC_SAMPLE_RATE      16000
#define MIC_BUFFER_BLOCKS    2   // Number of 0.5‑second blocks, adjust for latency
#define SAMPLE_RATE          MIC_SAMPLE_RATE
#define MAX_RECORD_SEC       12    // Expanded to 12s thanks to SPIRAM
#define MAX_RECORD_SAMP      (SAMPLE_RATE * MAX_RECORD_SEC)
#define VOLUME_MULTIPLIER    0.5f

// Hausa System Prompt
#define SYSTEM_PROMPT \
"You are Zarah, a Hausa-speaking shop debt assistant for a small business in Nigeria. " \
"You help a shop owner record debts, payments, and check customer accounts. " \
"Always understand informal Hausa speech, broken Hausa, mixed Hausa-English, and speech recognition mistakes. " \
"People may speak casually, indirectly, or with bad pronunciation. Infer meaning from context. " \
"Never ask unnecessary questions if intent is obvious. " \
\
"LANGUAGE RULES: " \
"Always reply in simple natural Hausa suitable for local shop owners. " \
"Be short, clear, and practical. " \
"Do not sound robotic or formal. " \
"Understand Hausa numbers including: dubu daya, dubu biyu, dubu goma, dari biyar, ashirin, talatin, hamsin, dari, miliyan. " \
"Convert spoken Hausa money amounts into digits whenever possible. " \
\
"UNDERSTAND THESE DEBT PHRASES AS THE SAME THING: " \
"'ya karba', 'ya dauka', 'ya ci bashi', 'ka rubuta', 'ka saka masa', 'ka kara masa', 'ya tafi da', 'ya debo', 'ya amsa', 'ya zo ya dauki', means GOODS TAKEN ON CREDIT. " \
\
"UNDERSTAND THESE PAYMENT PHRASES AS PAYMENT: " \
"'ya turo', 'ya bayar', 'ya biya', 'ya kawo kudi', 'ya rage', 'ya kawo wani abu', 'ya kawo kudin', means CUSTOMER PAID MONEY. " \
\
"UNDERSTAND THESE ACCOUNT QUERIES: " \
"'nawa bashinsa?', 'nawa ake binsa?', 'duba lissafi', 'nawa ya rage?', 'ina lissafin', 'yaya lissafin', 'account dinsa', 'me ake binsa?', means ACCOUNT CHECK. " \
\
"UNDERSTAND THESE ALL-DEBT QUERIES: " \
"'waye ake bi?', 'waye ke da bashi?', 'su waye ake binsu?', 'duk masu bashi', 'all debtors', means FETCH ALL ACCOUNTS. " \
\
"COMMAND RULES: " \
"If user says someone took goods AND mentions money amount, reply ONLY in this exact format: " \
"[RECORD_MONEY:Name:Amount:Item] Hausa confirmation. " \
"Example: [RECORD_MONEY:Musa:10000:Shinkafa] Na rubuta cewa Musa ya karbi shinkafa ta naira dubu goma. " \
\
"If user says someone took goods AND only quantity is mentioned, reply ONLY in this exact format: " \
"[RECORD_ITEM:Name:Quantity:Item] Hausa confirmation. " \
"Example: [RECORD_ITEM:Musa:6:Turare] Na rubuta cewa Musa ya karbi turare guda shida. " \
\
"If customer paid money, reply ONLY in this exact format: " \
"[PAY:Name:Amount] Hausa confirmation. " \
"Example: [PAY:Musa:5000] Na rubuta cewa Musa ya biya naira dubu biyar. " \
\
"If user asks for a person's account, reply EXACTLY: " \
"[FETCH:Name] " \
"and NOTHING else. " \
\
"If user asks for all debtors, reply EXACTLY: " \
"[FETCH:ALL] " \
"and NOTHING else. " \
\
"IMPORTANT UNDERSTANDING RULES: " \
"If item name is unclear from speech recognition, infer the most likely shop item from context. " \
"If pronunciation is imperfect, infer the intended customer name. " \
"If user forgets item but clearly means debt, still record if amount exists. " \
"If user forgets amount but quantity exists, use RECORD_ITEM. " \
"If user intent is obvious, DO NOT ask follow-up questions. " \
"Be tolerant of STT mistakes and broken sentences. " \
\
"STRICT OUTPUT RULES: " \
"Never explain your reasoning. " \
"Never return JSON. " \
"Never change the command format. " \
"Never add extra English. " \
"Always prioritize extracting useful structured data from messy speech. " \
"CRITICAL: Normal replies MUST start with [HAPPY] or [NEUTRAL]."

#define MAX_USER_MSG     512
#define MAX_RESPONSE     2048
#define HTTP_BUF_SIZE    16384 // Large response buffer in PSRAM

// Global State
volatile int current_led_state = 0; // 0=OFF, 1=ON, 2=BLINK
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static spi_device_handle_t spi_handle;
static volatile bool speaker_is_enabled = false;
static sdmmc_card_t *card = NULL;

static char wifi_ip_addr[32] = "0.0.0.0";
static char last_action_summary[64] = "Babu";

// ─── ST7735 SPI LCD DRIVER ───────────────────────────────────────────────────

static void lcd_write_cmd(uint8_t cmd) {
    gpio_set_level(LCD_GPIO_DC, 0);
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    spi_device_polling_transmit(spi_handle, &t);
}

static void lcd_write_data(const uint8_t *data, size_t len) {
    if (len == 0) return;
    gpio_set_level(LCD_GPIO_DC, 1);
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(spi_handle, &t);
}

static void lcd_write_byte(uint8_t val) {
    lcd_write_data(&val, 1);
}

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    lcd_write_cmd(0x2A); // Columns
    uint8_t data_col[] = {
        (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
        (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF)
    };
    lcd_write_data(data_col, 4);

    lcd_write_cmd(0x2B); // Rows
    uint8_t data_row[] = {
        (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
        (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF)
    };
    lcd_write_data(data_row, 4);

    lcd_write_cmd(0x2C); // Memory Write
}

static void lcd_clear(uint16_t color) {
    lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    uint16_t *line_buf = malloc(LCD_WIDTH * sizeof(uint16_t));
    if (!line_buf) return;
    
    uint16_t le_color = (color << 8) | (color >> 8); // Swap to big-endian
    for (int i = 0; i < LCD_WIDTH; i++) {
        line_buf[i] = le_color;
    }
    for (int r = 0; r < LCD_HEIGHT; r++) {
        lcd_write_data((uint8_t *)line_buf, LCD_WIDTH * sizeof(uint16_t));
    }
    free(line_buf);
}

static void lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color) {
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return;
    lcd_set_window(x, y, x, y);
    uint16_t c = (color << 8) | (color >> 8);
    lcd_write_data((uint8_t *)&c, 2);
}

static void lcd_draw_char(int x, int y, char c, uint16_t color, uint16_t bg, int scale) {
    if (c < 32 || c > 127) c = ' ';
    int font_idx = c - 32;

    if (scale == 1) {
        if (x + 5 >= LCD_WIDTH || y + 7 >= LCD_HEIGHT || x < 0 || y < 0) return;
        lcd_set_window(x, y, x + 4, y + 6);
        uint16_t char_buf[35];
        uint16_t le_color = (color << 8) | (color >> 8);
        uint16_t le_bg = (bg << 8) | (bg >> 8);

        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                uint8_t byte = System5x7[font_idx * 5 + col];
                if (byte & (1 << row)) {
                    char_buf[row * 5 + col] = le_color;
                } else {
                    char_buf[row * 5 + col] = le_bg;
                }
            }
        }
        lcd_write_data((uint8_t *)char_buf, 35 * sizeof(uint16_t));
    } else {
        for (int col = 0; col < 5; col++) {
            uint8_t byte = System5x7[font_idx * 5 + col];
            for (int row = 0; row < 7; row++) {
                uint16_t current_color = (byte & (1 << row)) ? color : bg;
                for (int sx = 0; sx < scale; sx++) {
                    for (int sy = 0; sy < scale; sy++) {
                        lcd_draw_pixel(x + col * scale + sx, y + row * scale + sy, current_color);
                    }
                }
            }
        }
    }
}

static void lcd_draw_string(int x, int y, const char *str, uint16_t color, uint16_t bg, int scale) {
    int cx = x;
    int cy = y;
    while (*str) {
        if (*str == '\n') {
            cx = x;
            cy += 8 * scale;
        } else {
            lcd_draw_char(cx, cy, *str, color, bg, scale);
            cx += 6 * scale;
            if (cx + 5 * scale >= LCD_WIDTH) {
                cx = x;
                cy += 8 * scale;
            }
        }
        str++;
    }
}

static void lcd_update_state(const char *state_title, uint16_t theme_color, const char *line1, const char *line2, const char *line3) {
    lcd_clear(RGB_DARKGRAY);
    
    // Draw top title bar background
    for (int y = 0; y < 20; y++) {
        for (int x = 0; x < LCD_WIDTH; x++) {
            lcd_draw_pixel(x, y, theme_color);
        }
    }
    lcd_draw_string(35, 6, "ZARAH KASUWANCI", RGB_WHITE, theme_color, 1);
    
    // Draw status title centered dynamically
    int title_len = strlen(state_title);
    int title_x = (LCD_WIDTH - (title_len * 12 - 2)) / 2;
    if (title_x < 0) title_x = 0;
    lcd_draw_string(title_x, 32, state_title, theme_color, RGB_DARKGRAY, 2);
    
    // Draw detail lines
    if (line1) lcd_draw_string(6, 75, line1, RGB_WHITE, RGB_DARKGRAY, 1);
    if (line2) lcd_draw_string(6, 95, line2, RGB_YELLOW, RGB_DARKGRAY, 1);
    if (line3) lcd_draw_string(6, 115, line3, RGB_CYAN, RGB_DARKGRAY, 1);
}

static void init_spi_lcd(void) {
    gpio_set_direction(LCD_GPIO_DC, GPIO_MODE_OUTPUT);
    if (LCD_GPIO_RST >= 0) {
        gpio_set_direction(LCD_GPIO_RST, GPIO_MODE_OUTPUT);
    }
    
    spi_bus_config_t buscfg = {
        .miso_io_num = -1,
        .mosi_io_num = LCD_GPIO_MOSI,
        .sclk_io_num = LCD_GPIO_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * 2,
    };
    
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 15 * 1000 * 1000, // 15 MHz as in the working setup
        .mode = 0,
        .spics_io_num = LCD_GPIO_CS,
        .queue_size = 7,
    };
    
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &devcfg, &spi_handle));

    // ST7735 Reset Sequence
    if (LCD_GPIO_RST >= 0) {
        gpio_set_level(LCD_GPIO_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(LCD_GPIO_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
    } else {
        // Tied to physical Reset line, wait for reset sequence to settle
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // ST7735 Minimal Working Initialization Sequence
    lcd_write_cmd(0x01); // Software Reset
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_write_cmd(0x11); // Sleep Out
    vTaskDelay(pdMS_TO_TICKS(200));

    lcd_write_cmd(0x3A); // Interface Pixel Format
    uint8_t c1 = 0x05;   // 16-bit color
    lcd_write_data(&c1, 1);

    lcd_write_cmd(0x36); // MADCTL Orientation (Landscape, BGR mode)
    uint8_t c2 = 0xA8;   // Matching working project exactly
    lcd_write_data(&c2, 1);

    lcd_write_cmd(0x21); // Display Inversion On
    lcd_write_cmd(0x29); // Display On
    vTaskDelay(pdMS_TO_TICKS(100));

    lcd_clear(RGB_DARKGRAY);
    ESP_LOGI(TAG, "ST7735 LCD Driver Initialized");
}

// ─── SD CARD STORAGE & JSON LEDGER ──────────────────────────────────────────

static esp_err_t init_sd_card(void) {
    ESP_LOGI(TAG, "Mounting SD card via SDMMC...");
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_PROBING; // safe low frequency for init
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = GPIO_NUM_39;
    slot_config.cmd = GPIO_NUM_38;
    slot_config.d0 = GPIO_NUM_40;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    slot_config.d3 = -1;
    
    // Mount the SD card. No need to manipulate the task watchdog —
    // esp_task_wdt_deinit() is dangerous (can corrupt WDT state) and
    // does NOT prevent the Interrupt WDT from firing anyway.
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount filesystem: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "SD Card mounted successfully.");
    return ESP_OK;
}

static void sd_save_transaction(const char *customer, int amount, const char *item, char type) {
    char filepath[64];
    char name_lower[32];
    int len = strlen(customer);
    if (len > 30) len = 30;
    for (int i = 0; i < len; i++) {
        name_lower[i] = tolower((unsigned char)customer[i]);
    }
    name_lower[len] = '\0';
    snprintf(filepath, sizeof(filepath), "/sdcard/%s.txt", name_lower);

    cJSON *root = NULL;
    cJSON *tx_array = NULL;

    FILE *f = fopen(filepath, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = malloc(size + 1);
        if (buf) {
            size_t bytes_read = fread(buf, 1, size, f);
            buf[bytes_read] = '\0';
            root = cJSON_Parse(buf);
            free(buf);
        }
        fclose(f);
    }

    if (!root) {
        root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", customer);
        tx_array = cJSON_AddArrayToObject(root, "transactions");
    } else {
        tx_array = cJSON_GetObjectItem(root, "transactions");
        if (!tx_array) {
            tx_array = cJSON_AddArrayToObject(root, "transactions");
        }
    }

    cJSON *tx = cJSON_CreateObject();
    time_t now;
    time(&now);
    cJSON_AddNumberToObject(tx, "ts", (double)now);
    
    char type_str[2] = {type, '\0'};
    cJSON_AddStringToObject(tx, "type", type_str);
    cJSON_AddNumberToObject(tx, "amount", amount);
    cJSON_AddStringToObject(tx, "item", item ? item : "");

    cJSON_AddItemToArray(tx_array, tx);

    // Limit transactions list to keep file parsing fast
    if (cJSON_GetArraySize(tx_array) > 100) {
        cJSON_DeleteItemFromArray(tx_array, 0);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        f = fopen(filepath, "w");
        if (f) {
            fputs(json_str, f);
            fclose(f);
            ESP_LOGI(TAG, "Saved transaction to %s", filepath);
        } else {
            ESP_LOGE(TAG, "Failed to open %s for writing", filepath);
        }
        free(json_str);
    }

    cJSON_Delete(root);

    // Update global visual summary
    if (type == 'T') {
        snprintf(last_action_summary, sizeof(last_action_summary), "%s: +N%d", customer, amount);
    } else if (type == 'U') {
        snprintf(last_action_summary, sizeof(last_action_summary), "%s: +%d pcs", customer, amount);
    } else if (type == 'P') {
        snprintf(last_action_summary, sizeof(last_action_summary), "%s: -N%d", customer, amount);
    }
}

static void sd_get_customer_history(const char *customer, char *out_history_text, size_t max_out) {
    char filepath[64];
    char name_lower[32];
    int len = strlen(customer);
    if (len > 30) len = 30;
    for (int i = 0; i < len; i++) {
        name_lower[i] = tolower((unsigned char)customer[i]);
    }
    name_lower[len] = '\0';
    snprintf(filepath, sizeof(filepath), "/sdcard/%s.txt", name_lower);

    FILE *f = fopen(filepath, "r");
    if (!f) {
        snprintf(out_history_text, max_out, "%s bai da wani lissafi a yanzu.", customer);
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        snprintf(out_history_text, max_out, "Kuskure na tuna lissafi.");
        return;
    }
    size_t bytes_read = fread(buf, 1, size, f);
    buf[bytes_read] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        snprintf(out_history_text, max_out, "Kuskure wajen karanta lissafi.");
        return;
    }

    cJSON *tx_array = cJSON_GetObjectItem(root, "transactions");
    int total_debt = 0;
    out_history_text[0] = '\0';

    if (tx_array && cJSON_IsArray(tx_array)) {
        int tx_count = cJSON_GetArraySize(tx_array);
        for (int i = 0; i < tx_count; i++) {
            cJSON *tx = cJSON_GetArrayItem(tx_array, i);
            if (tx) {
                cJSON *ts_item = cJSON_GetObjectItem(tx, "ts");
                cJSON *type_item = cJSON_GetObjectItem(tx, "type");
                cJSON *amount_item = cJSON_GetObjectItem(tx, "amount");
                cJSON *item_item = cJSON_GetObjectItem(tx, "item");

                long ts = ts_item ? (long)ts_item->valuedouble : 0;
                char type = (type_item && type_item->valuestring) ? type_item->valuestring[0] : 'T';
                int amt = amount_item ? amount_item->valueint : 0;
                const char *item = (item_item && item_item->valuestring) ? item_item->valuestring : "";

                if (type == 'T' || type == 'U') {
                    total_debt += amt;
                } else if (type == 'P') {
                    total_debt -= amt;
                }

                if (i >= tx_count - 5) {
                    struct tm tinfo;
                    time_t time_val = (time_t)ts;
                    localtime_r(&time_val, &tinfo);
                    char date_str[32] = "-";
                    if (ts > 10000) {
                        strftime(date_str, sizeof(date_str), "%d %b", &tinfo);
                    }

                    char line[128];
                    if (type == 'T') {
                        snprintf(line, sizeof(line), "- Ranar %s: Ya kar6a %s (N%d)\n", date_str, item, amt);
                    } else if (type == 'U') {
                        snprintf(line, sizeof(line), "- Ranar %s: Ya kar6a %s (Guda %d)\n", date_str, item, amt);
                    } else if (type == 'P') {
                        snprintf(line, sizeof(line), "- Ranar %s: Ya turo (N%d)\n", date_str, amt);
                    }
                    if (strlen(out_history_text) + strlen(line) < max_out) {
                        strcat(out_history_text, line);
                    }
                }
            }
        }
    }

    if (total_debt < 0) total_debt = 0;

    char bal_line[128];
    snprintf(bal_line, sizeof(bal_line), "Total Debt (Bashin da ake binsa): N%d", total_debt);
    if (strlen(out_history_text) + strlen(bal_line) < max_out) {
        strcat(out_history_text, bal_line);
    }

    cJSON_Delete(root);
}

static void sd_get_all_debtors(char *out_buf, size_t buf_size) {
    DIR *dir = opendir("/sdcard");
    if (!dir) {
        snprintf(out_buf, buf_size, "Babu wani lissafi yanzu.");
        return;
    }

    out_buf[0] = '\0';
    bool found_any = false;
    size_t current_len = 0;
    struct dirent *de;

    while ((de = readdir(dir)) != NULL) {
        int len = strlen(de->d_name);
        if (len > 5 && strcmp(de->d_name + len - 5, ".txt") == 0) {
            char filepath[300];
            snprintf(filepath, sizeof(filepath), "/sdcard/%s", de->d_name);

            FILE *f = fopen(filepath, "r");
            if (f) {
                fseek(f, 0, SEEK_END);
                long size = ftell(f);
                fseek(f, 0, SEEK_SET);
                char *buf = malloc(size + 1);
                if (buf) {
                    size_t bytes_read = fread(buf, 1, size, f);
                    buf[bytes_read] = '\0';
                    cJSON *root = cJSON_Parse(buf);
                    if (root) {
                        cJSON *name_item = cJSON_GetObjectItem(root, "name");
                        cJSON *tx_array = cJSON_GetObjectItem(root, "transactions");
                        const char *fullname = name_item ? name_item->valuestring : de->d_name;

                        int total_debt = 0;
                        if (tx_array && cJSON_IsArray(tx_array)) {
                            int tx_count = cJSON_GetArraySize(tx_array);
                            for (int i = 0; i < tx_count; i++) {
                                cJSON *tx = cJSON_GetArrayItem(tx_array, i);
                                if (tx) {
                                    cJSON *type_item = cJSON_GetObjectItem(tx, "type");
                                    cJSON *amount_item = cJSON_GetObjectItem(tx, "amount");
                                    char type = (type_item && type_item->valuestring) ? type_item->valuestring[0] : 'T';
                                    int amt = amount_item ? amount_item->valueint : 0;
                                    if (type == 'T' || type == 'U') total_debt += amt;
                                    else if (type == 'P') total_debt -= amt;
                                }
                            }
                        }

                        if (total_debt > 0) {
                            found_any = true;
                            char entry_str[384];
                            snprintf(entry_str, sizeof(entry_str), "%s: N%d. ", fullname, total_debt);
                            if (current_len + strlen(entry_str) < buf_size - 1) {
                                strcat(out_buf, entry_str);
                                current_len += strlen(entry_str);
                            }
                        }
                        cJSON_Delete(root);
                    }
                    free(buf);
                }
                fclose(f);
            }
        }
    }
    closedir(dir);

    if (!found_any) {
        snprintf(out_buf, buf_size, "Babu kowa da bashi.");
    }
}

static void sd_delete_customer(const char *customer) {
    char filepath[64];
    char name_lower[32];
    int len = strlen(customer);
    if (len > 30) len = 30;
    for (int i = 0; i < len; i++) {
        name_lower[i] = tolower((unsigned char)customer[i]);
    }
    name_lower[len] = '\0';
    snprintf(filepath, sizeof(filepath), "/sdcard/%s.txt", name_lower);
    
    if (unlink(filepath) == 0) {
        ESP_LOGI(TAG, "Deleted customer ledger file: %s", filepath);
    } else {
        ESP_LOGE(TAG, "Failed to delete customer file %s", filepath);
    }
}

// ─── LEGACY I2S DRIVER INTEGRATION ─────────────────────────────

static void init_i2s_mic(void) {
    ESP_LOGI(TAG, "Initializing legacy I2S Mic...");
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 1024,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };
    i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = I2S_MIC_BCLK,
        .ws_io_num = I2S_MIC_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_MIC_DATA
    };
    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pin_config));
}

static void init_i2s_speaker(void) {
    ESP_LOGI(TAG, "Initializing stable I2S Speaker...");
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = 16000, // Matching mic sample rate prevents sync issues
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,   // Reduced from 16 to save memory/power
        .dma_buf_len = 256,   // Reduced to prevent Brownout
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };
    
    i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = I2S_SPK_BCLK,
        .ws_io_num = I2S_SPK_WS,
        .data_out_num = I2S_SPK_DATA,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_1, &pin_config);
    i2s_zero_dma_buffer(I2S_NUM_1);
}

static void speaker_set_sample_rate(uint32_t rate) {
    i2s_set_sample_rates(I2S_NUM_1, rate);
}

// ─── AUDIO RECORD & PLAYBACK ──────────────────────────────────────────────────

#define BLOCKS_PER_SEC   2
#define MAX_BLOCKS       (MAX_RECORD_SEC * BLOCKS_PER_SEC)
#define BLOCK_SAMPLES    (SAMPLE_RATE / BLOCKS_PER_SEC) // 16KB blocks

typedef struct {
    int16_t *blocks[MAX_BLOCKS];
    size_t total_samples;
} audio_buffer_t;

static audio_buffer_t record_audio(void) {
    audio_buffer_t buf = {0};
    ESP_LOGI(TAG, "Push and HOLD the BOOT button (GPIO 0) to talk...");
    
    char details[64];
    snprintf(details, sizeof(details), "IP: %s", wifi_ip_addr);
    lcd_update_state("SHIRYA", RGB_GREEN, "Danna BOOT don magana", "Sallama!", details);
    current_led_state = 0; 
    
    // Wait for button press
    while (gpio_get_level(BUTTON_PIN) == 1) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    // Flush I2S so old data doesn't build up
    size_t temp_read = 0;
    int16_t dummy[1024];
    i2s_read(I2S_NUM_0, dummy, sizeof(dummy), &temp_read, 0);
    
    ESP_LOGI(TAG, "Button pressed! Recording started.");
    lcd_update_state("JIN MAGANA", RGB_RED, "Saurara...", "Yi magana yanzu...", NULL);
    current_led_state = 1; 
    
    size_t samples_read = 0;
    int16_t i2s_read_buff[1024];

    while (1) {
        size_t bytes_read = 0;
        i2s_read(I2S_NUM_0, i2s_read_buff, sizeof(i2s_read_buff), &bytes_read, portMAX_DELAY);
        int frames = bytes_read / sizeof(int16_t);

        if (samples_read + frames > MAX_RECORD_SAMP) {
            ESP_LOGI(TAG, "Max record duration reached. Stopping.");
            break;
        }

        int frames_copied = 0;
        while(frames_copied < frames) {
            int cur_block = samples_read / BLOCK_SAMPLES;
            if (!buf.blocks[cur_block]) {
                buf.blocks[cur_block] = malloc(BLOCK_SAMPLES * sizeof(int16_t));
                if (!buf.blocks[cur_block]) {
                    ESP_LOGE(TAG, "Out of Memory allocating block %d!", cur_block);
                    buf.total_samples = samples_read;
                    return buf; 
                }
            }
            int offset_in_block = samples_read % BLOCK_SAMPLES;
            int space_in_block = BLOCK_SAMPLES - offset_in_block;
            int to_copy = (frames - frames_copied) < space_in_block ? (frames - frames_copied) : space_in_block;
            
            memcpy(&buf.blocks[cur_block][offset_in_block], &i2s_read_buff[frames_copied], to_copy * sizeof(int16_t));
            samples_read += to_copy;
            frames_copied += to_copy;
        }

        // Stop if button is released
        if (gpio_get_level(BUTTON_PIN) == 1) {
            ESP_LOGI(TAG, "Button released. Stopping recording.");
            break;
        }
    }

    buf.total_samples = samples_read;
    ESP_LOGI(TAG, "Recording finished. Total samples: %d", samples_read);
    lcd_update_state("TUNANI", RGB_MAGENTA, "Tana aiki...", "Gemini AI...", NULL);
    current_led_state = 1; 
    return buf;
}

// Base64 helper table
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int write_http_chunk(esp_http_client_handle_t client, const char *data, size_t len) {
    if (len == 0) return 0;
    return esp_http_client_write(client, data, len);
}

static void write_wav_base64_chunked(esp_http_client_handle_t client, const audio_buffer_t *audio) {
    struct {
        char riff[4];
        uint32_t overall_size;
        char wave[4];
        char fmt_chunk_marker[4];
        uint32_t length_of_fmt;
        uint16_t format_type;
        uint16_t channels;
        uint32_t sample_rate;
        uint32_t byte_rate;
        uint16_t block_align;
        uint16_t bits_per_sample;
        char data_chunk_header[4];
        uint32_t data_size;
    } wav_header;

    uint32_t data_bytes = audio->total_samples * sizeof(int16_t);
    memcpy(wav_header.riff, "RIFF", 4);
    wav_header.overall_size = data_bytes + 36;
    memcpy(wav_header.wave, "WAVE", 4);
    memcpy(wav_header.fmt_chunk_marker, "fmt ", 4);
    wav_header.length_of_fmt = 16;
    wav_header.format_type = 1;
    wav_header.channels = 1;
    wav_header.sample_rate = SAMPLE_RATE;
    wav_header.byte_rate = SAMPLE_RATE * 2;
    wav_header.block_align = 2;
    wav_header.bits_per_sample = 16;
    memcpy(wav_header.data_chunk_header, "data", 4);
    wav_header.data_size = data_bytes;

    uint8_t process_buf[3];
    int p_idx = 0;
    
    char send_buf[1024];
    int send_idx = 0;

    #define FLUSH_B64() \
        send_buf[send_idx++] = b64_table[(process_buf[0] >> 2) & 0x3F]; \
        send_buf[send_idx++] = b64_table[((process_buf[0] & 0x03) << 4) | ((process_buf[1] >> 4) & 0x0F)]; \
        send_buf[send_idx++] = b64_table[((process_buf[1] & 0x0F) << 2) | ((process_buf[2] >> 6) & 0x03)]; \
        send_buf[send_idx++] = b64_table[process_buf[2] & 0x3F]; \
        if (send_idx >= 1024) { \
            if (write_http_chunk(client, send_buf, 1024) < 0) return; \
            send_idx = 0; \
        } \
        p_idx = 0;

    uint8_t *hptr = (uint8_t *)&wav_header;
    for (int i = 0; i < sizeof(wav_header); i++) {
        process_buf[p_idx++] = hptr[i];
        if (p_idx == 3) { FLUSH_B64(); }
    }

    size_t samples_left = audio->total_samples;
    for(int b = 0; b < MAX_BLOCKS && samples_left > 0; b++) {
        if (!audio->blocks[b]) break;
        int samples_in_block = samples_left > BLOCK_SAMPLES ? BLOCK_SAMPLES : samples_left;
        uint8_t *dptr = (uint8_t *)audio->blocks[b];
        int bytes = samples_in_block * sizeof(int16_t);
        for (int i = 0; i < bytes; i++) {
            process_buf[p_idx++] = dptr[i];
            if (p_idx == 3) { FLUSH_B64(); }
        }
        samples_left -= samples_in_block;
    }

    if (p_idx == 1) {
        send_buf[send_idx++] = b64_table[(process_buf[0] >> 2) & 0x3F];
        send_buf[send_idx++] = b64_table[(process_buf[0] & 0x03) << 4];
        send_buf[send_idx++] = '=';
        send_buf[send_idx++] = '=';
    } else if (p_idx == 2) {
        send_buf[send_idx++] = b64_table[(process_buf[0] >> 2) & 0x3F];
        send_buf[send_idx++] = b64_table[((process_buf[0] & 0x03) << 4) | ((process_buf[1] >> 4) & 0x0F)];
        send_buf[send_idx++] = b64_table[((process_buf[1] & 0x0F) << 2)];
        send_buf[send_idx++] = '=';
    }

    if (send_idx > 0) write_http_chunk(client, send_buf, send_idx);
}

void play_test_tone(void) {
    ESP_LOGI(TAG, "Playing 440Hz test tone...");
    int sample_rate = 16000;
    int duration_sec = 1;
    int total_samples = sample_rate * duration_sec;
    int16_t *sine_buf = malloc(1024 * sizeof(int16_t));
    if (sine_buf) {
        speaker_set_sample_rate(16000);
        for (int chunk = 0; chunk < total_samples / 1024; chunk++) {
            for (int i = 0; i < 1024; i++) {
                float time_val = (float)((chunk * 1024) + i) / sample_rate;
                sine_buf[i] = (int16_t)(sin(2.0 * M_PI * 440.0 * time_val) * 32767.0 * VOLUME_MULTIPLIER);
            }
            size_t bytes_written;
            i2s_write(I2S_NUM_1, sine_buf, 1024 * sizeof(int16_t), &bytes_written, portMAX_DELAY);
        }
        i2s_zero_dma_buffer(I2S_NUM_1);
        free(sine_buf);
    }
}

// ─── GEMINI API CLIENTS ───────────────────────────────────────────────────────

static bool extract_claude_text(const char *json, char *out, size_t out_size) {
    const char *key = "\"text\": \"";
    char *pos = strstr(json, key);
    if (!pos) {
        key = "\"text\":\"";
        pos = strstr(json, key);
    }
    if (!pos) return false;

    pos += strlen(key);
    const char *start = pos;
    const char *end = pos;

    while (*end && *end != '"') {
        if (*end == '\\' && *(end + 1) != '\0') { end += 2; } 
        else { end++; }
    }

    size_t len = end - start;
    if (len >= out_size) len = out_size - 1;
    
    int ci = 0;
    for (size_t i = 0; i < len && ci < (int)out_size - 1; i++) {
        if (start[i] == '\\' && i + 1 < len) {
            if (start[i+1] == 'n') { out[ci++] = ' '; i++; } 
            else if (start[i+1] == '"') { out[ci++] = '"'; i++; } 
            else if (start[i+1] == '\\') { out[ci++] = '\\'; i++; } 
            else { out[ci++] = start[i+1]; i++; }
        } else {
            out[ci++] = start[i];
        }
    }
    out[ci] = '\0';
    return (ci > 0);
}

static bool speech_to_text(const audio_buffer_t *audio, char* out_text, size_t out_size) {
    ESP_LOGI(TAG, "Initiating Gemini Multimodal STT...");
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "/v1beta/models/%s:generateContent?key=%s", GEMINI_MODEL, GEMINI_API_KEY);

    const char* req_prefix = "{\"system_instruction\":{\"parts\":[{\"text\":\"" SYSTEM_PROMPT "\"}]},\"contents\":[{\"parts\":[{\"text\":\"The user just said the following audio message. Transcribe it and reply to it directly as Zarah would. Return ONLY your reply to the user, nothing else.\"},{\"inlineData\":{\"mimeType\":\"audio/wav\",\"data\":\"";
    const char* req_suffix = "\"}}]}]}";

    size_t raw_bytes = audio->total_samples * sizeof(int16_t) + 44;
    size_t b64_len = ((raw_bytes + 2) / 3) * 4;
    int content_length = strlen(req_prefix) + b64_len + strlen(req_suffix);

    bool success = false;
    int max_retries = 3;

    for (int attempt = 1; attempt <= max_retries; attempt++) {
        esp_http_client_config_t config = {
            .host = GEMINI_HOST,
            .port = GEMINI_PORT,
            .path = full_path,
            .method = HTTP_METHOD_POST,
            .transport_type = HTTP_TRANSPORT_OVER_SSL,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 30000,
            .buffer_size = 2048,
            .buffer_size_tx = 4096,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        esp_http_client_set_header(client, "Content-Type", "application/json");

        esp_err_t err = esp_http_client_open(client, content_length);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "STT Connect failed (Attempt %d/%d): %s", attempt, max_retries, esp_err_to_name(err));
            esp_http_client_cleanup(client);
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }

        write_http_chunk(client, req_prefix, strlen(req_prefix));
        write_wav_base64_chunked(client, audio);
        write_http_chunk(client, req_suffix, strlen(req_suffix));
        
        ESP_LOGI(TAG, "STT payload sent, waiting for response...");
        err = esp_http_client_fetch_headers(client);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "STT Fetch headers failed (Attempt %d/%d)", attempt, max_retries);
            esp_http_client_cleanup(client);
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }

        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "STT HTTP Status: %d", status);

        if (status == 200) {
            char *resp_buf = calloc(1, HTTP_BUF_SIZE);
            if (resp_buf) {
                int read_len = esp_http_client_read(client, resp_buf, HTTP_BUF_SIZE - 1);
                if (read_len > 0 && extract_claude_text(resp_buf, out_text, out_size)) {
                    ESP_LOGI(TAG, "STT+AI Reply: %s", out_text);
                    success = true;
                }
                free(resp_buf);
            }
        } else {
             ESP_LOGW(TAG, "STT failed with status %d on attempt %d", status, attempt);
             char *err_resp = calloc(1, 2048);
             if (err_resp) {
                 int read_len = esp_http_client_read(client, err_resp, 2047);
                 if (read_len > 0) {
                     ESP_LOGE(TAG, "STT Error Response Body: %s", err_resp);
                 }
                 free(err_resp);
             }
        }
        
        esp_http_client_cleanup(client);
        if (success) break;
        
        if (attempt < max_retries) {
            ESP_LOGI(TAG, "Retrying STT in 2 seconds...");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    return success;
}

static bool gemini_text_to_text(const char *prompt, char *out_text, size_t out_size) {
    ESP_LOGI(TAG, "Requesting Gemini AI Text-to-Text (Turn 2)...");
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "/v1beta/models/%s:generateContent?key=%s", GEMINI_MODEL, GEMINI_API_KEY);

    esp_http_client_config_t config = {
        .host = GEMINI_HOST,
        .port = GEMINI_PORT,
        .path = full_path,
        .method = HTTP_METHOD_POST,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 2048,
        .buffer_size_tx = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    char safe_prompt[1024] = {0};
    int si = 0;
    for(int i = 0; prompt[i] && si < 1022; i++) {
        if(prompt[i] == '"') { safe_prompt[si++]='\\'; safe_prompt[si++]='"'; }
        else if(prompt[i] == '\n') { safe_prompt[si++]=' '; }
        else { safe_prompt[si++] = prompt[i]; }
    }

    char *req_json = malloc(2048);
    if (!req_json) {
        esp_http_client_cleanup(client);
        return false;
    }
    snprintf(req_json, 2048, "{\"contents\":[{\"parts\":[{\"text\":\"%s\"}]}]}", safe_prompt);

    esp_err_t err = esp_http_client_open(client, strlen(req_json));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTPS connection for T2T");
        free(req_json);
        esp_http_client_cleanup(client);
        return false;
    }
    
    esp_http_client_write(client, req_json, strlen(req_json));
    esp_http_client_fetch_headers(client);
    
    bool success = false;
    int status = esp_http_client_get_status_code(client);
    if (status == 200) {
        char *resp_buf = calloc(1, HTTP_BUF_SIZE);
        if (resp_buf) {
            int read_len = esp_http_client_read(client, resp_buf, HTTP_BUF_SIZE - 1);
            if (read_len > 0 && extract_claude_text(resp_buf, out_text, out_size)) {
                ESP_LOGI(TAG, "T2T AI Reply: %s", out_text);
                success = true;
            }
            free(resp_buf);
        }
    } else {
         ESP_LOGE(TAG, "T2T failed with status %d", status);
    }
    
    free(req_json);
    esp_http_client_cleanup(client);
    return success;
}

static RingbufHandle_t tts_ringbuf = NULL;
static volatile bool tts_streaming = false;
static volatile bool tts_finished = true;

static void tts_player_task(void *arg) {
    size_t item_size;
    while (1) {
        uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(tts_ringbuf, &item_size, pdMS_TO_TICKS(50), 4096);
        if (data) {
            size_t bytes_written;
            i2s_write(I2S_NUM_1, data, item_size, &bytes_written, portMAX_DELAY);
            vRingbufferReturnItem(tts_ringbuf, (void *)data);
        } else if (!tts_streaming) {
            break;
        }
    }
    tts_finished = true;
    vTaskDelete(NULL);
}


static void text_to_speech_and_play(const char *text) {
    ESP_LOGI(TAG, "Requesting Gemini AI TTS...");
    current_led_state = 2; // Blinking LED
    lcd_update_state("MAGANA", RGB_ORANGE, "Zarah na magana...", last_action_summary, NULL);

    char full_path[256];
    snprintf(full_path, sizeof(full_path), "/v1beta/models/%s:generateContent?key=%s", TTS_MODEL, GEMINI_API_KEY);

    char *req_json = malloc(MAX_RESPONSE + 512);
    if (!req_json) return;
    
    // Strip tags
    char stripped_text[MAX_RESPONSE];
    {
        int di = 0;
        int ti = 0;
        while (text[ti] && di < MAX_RESPONSE - 2) {
            if (text[ti] == '[') {
                while (text[ti] && text[ti] != ']') ti++;
                if (text[ti] == ']') ti++;
                if (text[ti] == ' ') ti++;
            } else {
                stripped_text[di++] = text[ti++];
            }
        }
        stripped_text[di] = '\0';
    }
    
    char safe_text[MAX_RESPONSE];
    int si = 0;
    for(int i = 0; stripped_text[i] && si < MAX_RESPONSE - 2; i++) {
        if(stripped_text[i] == '"') { safe_text[si++]='\\'; safe_text[si++]='"'; }
        else if(stripped_text[i] == '\n') { safe_text[si++]=' '; }
        else { safe_text[si++] = stripped_text[i]; }
    }
    safe_text[si] = '\0';
    
    ESP_LOGI(TAG, "TTS Input: %s", safe_text);

    sprintf(req_json, "{\"contents\":[{\"parts\":[{\"text\":\"%s\"}]}],\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],\"speechConfig\":{\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":\"Kore\"}}}}}", safe_text);

    esp_http_client_config_t config = {
        .host = GEMINI_HOST,
        .port = GEMINI_PORT,
        .path = full_path,
        .method = HTTP_METHOD_POST,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 16384,
        .buffer_size_tx = 4096,
    };

    esp_http_client_handle_t client = NULL;
    int tts_status = 0;
    int max_tts_retries = 3;
    bool tts_connected = false;

    for (int attempt = 1; attempt <= max_tts_retries; attempt++) {
        client = esp_http_client_init(&config);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, req_json, strlen(req_json));

        esp_err_t err = esp_http_client_open(client, strlen(req_json));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open TTS connection (Attempt %d)", attempt);
            esp_http_client_cleanup(client);
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }

        esp_http_client_write(client, req_json, strlen(req_json));
        err = esp_http_client_fetch_headers(client);
        
        if (err == ESP_OK) {
            tts_status = esp_http_client_get_status_code(client);
            ESP_LOGI(TAG, "TTS HTTP Status: %d", tts_status);
            if (tts_status == 200) {
                tts_connected = true;
                break;
            } else {
                char *err_resp = calloc(1, 2048);
                if (err_resp) {
                    int read_len = esp_http_client_read(client, err_resp, 2047);
                    if (read_len > 0) {
                        ESP_LOGE(TAG, "TTS Error Response Body: %s", err_resp);
                    }
                    free(err_resp);
                }
            }
        }
        
        ESP_LOGW(TAG, "TTS failed. Retrying in 2 seconds...");
        esp_http_client_cleanup(client);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    if (!tts_connected) {
        ESP_LOGE(TAG, "TTS completely failed.");
        free(req_json);
        return;
    }

    int read_len;
    #define TTS_PCM_BUF_SIZE 2048
    char *chunk = malloc(4096);
    uint8_t *pcm_buf = malloc(TTS_PCM_BUF_SIZE);
    int pcm_idx = 0;
    uint32_t total_decoded = 0;
    bool skip_header = false;
    int state = 0;
    bool found_audio = false;
    char b64_chunk[4];
    int b64_idx = 0;
    
    if (!pcm_buf || !chunk) goto decode_done;

    speaker_set_sample_rate(24000);

    tts_ringbuf = xRingbufferCreate(65536, RINGBUF_TYPE_BYTEBUF);
    tts_streaming = true;
    tts_finished = false;
    if (tts_ringbuf) {
        xTaskCreatePinnedToCore(tts_player_task, "tts_player", 4096, NULL, 10, NULL, 1);
    } else {
        tts_finished = true;
        ESP_LOGE(TAG, "Failed to create TTS ringbuf!");
    }

    while ((read_len = esp_http_client_read(client, chunk, 4096)) > 0) {
        for (int i = 0; i < read_len; i++) {
            char c = chunk[i];
            
            if (!found_audio) {
                if (state == 0 && c == '"') state = 1;
                else if (state == 1 && c == 'd') state = 2;
                else if (state == 2 && c == 'a') state = 3;
                else if (state == 3 && c == 't') state = 4;
                else if (state == 4 && c == 'a') state = 5;
                else if (state == 5 && c == '"') state = 6;
                else if (state == 6 && (c == ':' || c == ' ')) state = (c == ':') ? 7 : 6;
                else if (state == 7 && (c == ' ' || c == '"')) {
                    if (c == '"') found_audio = true;
                } else {
                    state = (c == '"') ? 1 : 0;
                }
            } else {
                if (c == '"' || c == '\n') goto decode_done; 
                
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || 
                    (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '-' || c == '_' || c == '=') {
                    
                    b64_chunk[b64_idx++] = c;
                    if (b64_idx == 4) {
                        uint32_t val = 0;
                        int eq_count = 0;
                        for(int j=0; j<4; j++) {
                            if (b64_chunk[j] == '=') eq_count++;
                            else {
                                int v = -1;
                                if (b64_chunk[j] >= 'A' && b64_chunk[j] <= 'Z') v = b64_chunk[j] - 'A';
                                else if (b64_chunk[j] >= 'a' && b64_chunk[j] <= 'z') v = b64_chunk[j] - 'a' + 26;
                                else if (b64_chunk[j] >= '0' && b64_chunk[j] <= '9') v = b64_chunk[j] - '0' + 52;
                                else if (b64_chunk[j] == '+' || b64_chunk[j] == '-') v = 62;
                                else if (b64_chunk[j] == '/' || b64_chunk[j] == '_') v = 63;
                                val = (val << 6) | (v & 0x3F);
                            }
                        }
                        if (eq_count) val <<= (6 * eq_count);
                        
                        uint8_t dec[3];
                        dec[0] = (val >> 16) & 0xFF; dec[1] = (val >> 8) & 0xFF; dec[2] = val & 0xFF;

                        int valid_bytes = 3 - eq_count;
                        for (int k = 0; k < valid_bytes; k++) {
                            uint8_t byte = dec[k];
                            total_decoded++;
                            
                            if (total_decoded == 1 && byte == 'R') skip_header = true;
                            if (skip_header && total_decoded <= 44) continue; 
                            
                            pcm_buf[pcm_idx++] = byte;
                            
                            if (pcm_idx >= TTS_PCM_BUF_SIZE) {
                                int16_t *samples = (int16_t *)pcm_buf;
                                int num_samples = TTS_PCM_BUF_SIZE / 2;
                                for (int s = 0; s < num_samples; s++) {
                                    samples[s] = (int16_t)(samples[s] * VOLUME_MULTIPLIER);
                                }
                                if (tts_ringbuf) {
                                    xRingbufferSend(tts_ringbuf, pcm_buf, TTS_PCM_BUF_SIZE, portMAX_DELAY);
                                }
                                pcm_idx = 0;
                            }
                        }
                        b64_idx = 0;
                    }
                }
            }
        }
    }

decode_done:
    ESP_LOGI(TAG, "Decoding Complete. Total PCM parsed: %d", total_decoded);
    
    if (pcm_idx > 0 && pcm_buf && tts_ringbuf) {
        int16_t *samples = (int16_t *)pcm_buf;
        int num_samples = pcm_idx / 2;
        for (int s = 0; s < num_samples; s++) {
            samples[s] = (int16_t)(samples[s] * VOLUME_MULTIPLIER);
        }
        xRingbufferSend(tts_ringbuf, pcm_buf, pcm_idx, portMAX_DELAY);
    }
    
    tts_streaming = false;
    if (tts_ringbuf) {
        while (!tts_finished) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        vRingbufferDelete(tts_ringbuf);
        tts_ringbuf = NULL;
    }
    
    if (pcm_buf) free(pcm_buf);
    
    speaker_set_sample_rate(SAMPLE_RATE);

    free(chunk);
    free(req_json);
    esp_http_client_cleanup(client);
    
    char details[64];
    snprintf(details, sizeof(details), "IP: %s", wifi_ip_addr);
    lcd_update_state("SHIRYA", RGB_GREEN, "Danna BOOT don magana", "Sallama!", details);
}

// ─── COMMAND PARSER & TURN 2 PREP ─────────────────────────────────────────────

static bool needs_turn2 = false;
static char turn2_prompt[1024] = {0};

static void parse_and_execute_command(const char *reply, char *final_response, size_t resp_size) {
    strncpy(final_response, reply, resp_size - 1);
    final_response[resp_size - 1] = '\0';
    needs_turn2 = false;

    char *record_money_ptr = strstr(final_response, "[RECORD_MONEY:");
    char *record_item_ptr = strstr(final_response, "[RECORD_ITEM:");
    char *fetch_all_ptr = strstr(final_response, "[FETCH:ALL]");
    char *fetch_ptr = strstr(final_response, "[FETCH:");
    char *pay_ptr = strstr(final_response, "[PAY:");

    if (record_money_ptr) {
        char customer[32] = {0}, item[32] = {0};
        int amount = 0;
        if (sscanf(record_money_ptr, "[RECORD_MONEY:%31[^:]:%d:%31[^]]]", customer, &amount, item) == 3) {
            sd_save_transaction(customer, amount, item, 'T');
        }
    } else if (record_item_ptr) {
        char customer[32] = {0}, item[32] = {0};
        int qty = 0;
        if (sscanf(record_item_ptr, "[RECORD_ITEM:%31[^:]:%d:%31[^]]]", customer, &qty, item) == 3) {
            sd_save_transaction(customer, qty, item, 'U');
        }
    } else if (fetch_all_ptr) {
        char debtors[512] = {0};
        sd_get_all_debtors(debtors, sizeof(debtors));
        snprintf(turn2_prompt, sizeof(turn2_prompt), "Here is the list of all debts: %s. Summarize this briefly in Hausa directly to the user. Use the phrase 'Ana bin [Name] bashi naira [Amount]'.", debtors);
        needs_turn2 = true;
    } else if (fetch_ptr && !fetch_all_ptr) {
        char customer[32] = {0};
        if (sscanf(fetch_ptr, "[FETCH:%31[^]]]", customer) == 1) {
            char history[512] = {0};
            sd_get_customer_history(customer, history, sizeof(history));
            snprintf(turn2_prompt, sizeof(turn2_prompt), "Here is %s's history:\n%s\nSummarize this to the user in Hausa. Say what he took ('ya kar6a') and what he paid ('ya turo'). CRITICAL: For the final total, you MUST say 'ragowar bashin da aka biyo %s shine...' followed by the amount. Keep it to 2 clear sentences.", customer, history, customer);
            needs_turn2 = true;
        }
    } else if (pay_ptr) {
        char customer[32] = {0};
        int amount = 0;
        if (sscanf(pay_ptr, "[PAY:%31[^:]:%d]", customer, &amount) >= 2) {
            sd_save_transaction(customer, amount, "", 'P');
        }
    }

    if (!needs_turn2) {
        // Strip out tags
        char stripped[MAX_RESPONSE] = {0};
        int di = 0;
        int ti = 0;
        while (final_response[ti] && di < MAX_RESPONSE - 1) {
            if (final_response[ti] == '[') {
                while (final_response[ti] && final_response[ti] != ']') ti++;
                if (final_response[ti] == ']') ti++;
                if (final_response[ti] == ' ') ti++;
            } else {
                stripped[di++] = final_response[ti++];
            }
        }
        stripped[di] = '\0';
        
        strncpy(final_response, stripped, resp_size - 1);
        final_response[resp_size - 1] = '\0';
    }
}

// ─── CORE VOICE PIPELINE TASK ────────────────────────────────────────────────

static void voice_pipeline_task(void *arg) {
    ESP_LOGI(TAG, "Voice Pipeline Task Started");
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    while (1) {
        audio_buffer_t audio_buf = record_audio();

        if (audio_buf.total_samples > 0) {
            char reply[MAX_RESPONSE] = {0};
            if (speech_to_text(&audio_buf, reply, sizeof(reply))) {
                char final_response[MAX_RESPONSE] = {0};
                parse_and_execute_command(reply, final_response, sizeof(final_response));
                
                // Release recording RAM immediately (VAD buffer release)
                for(int i = 0; i < MAX_BLOCKS; i++) {
                    if (audio_buf.blocks[i]) {
                        free(audio_buf.blocks[i]);
                        audio_buf.blocks[i] = NULL;
                    }
                }

                if (needs_turn2) {
                    char turn2_reply[MAX_RESPONSE] = {0};
                    if (gemini_text_to_text(turn2_prompt, turn2_reply, sizeof(turn2_reply))) {
                        text_to_speech_and_play(turn2_reply);
                    } else {
                        text_to_speech_and_play("Yi hakuri, ba zan iya duba lissafin ba yanzu.");
                    }
                } else {
                    text_to_speech_and_play(final_response);
                }
            }
        }

        // Cleanup safety
        for(int i = 0; i < MAX_BLOCKS; i++) {
            if (audio_buf.blocks[i]) {
                free(audio_buf.blocks[i]);
                audio_buf.blocks[i] = NULL;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ─── WIFI & WEB SERVER (PREMIUM CARD-GRID WEB UI) ────────────────────────────

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data) {
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, retrying...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(wifi_ip_addr, sizeof(wifi_ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Connected! IP: %s", wifi_ip_addr);
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void load_wifi_credentials(char *ssid, char *pass) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("wifi_data", NVS_READONLY, &my_handle);
    if (err == ESP_OK) {
        size_t len = 32;
        nvs_get_str(my_handle, "ssid", ssid, &len);
        len = 64;
        nvs_get_str(my_handle, "pass", pass, &len);
        nvs_close(my_handle);
    }
}

static void save_wifi_credentials(const char *ssid, const char *pass) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("wifi_data", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        nvs_set_str(my_handle, "ssid", ssid);
        nvs_set_str(my_handle, "pass", pass);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}

static void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && (isxdigit((int)a) && isxdigit((int)b))) {
            if (a >= 'a') a -= 'a'-'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a'-'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            *dst++ = 16*a+b;
            src+=3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst++ = '\0';
}

static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    
    char *chunk = malloc(4096);
    if (!chunk) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    strcpy(chunk, "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'><title>Zarah Setup</title>"
                 "<link href='https://fonts.googleapis.com/css2?family=Outfit:wght@400;600;800&display=swap' rel='stylesheet'>"
                 "<style>:root{--bg:#0f0f12;--card-bg:rgba(255,255,255,0.05);--border:rgba(255,255,255,0.1);--text:#ffffff;--text-muted:#b3b3b3;--accent-debt:#ff5252;--accent-pay:#69f0ae;--primary:#2979ff;} "
                 "body{font-family:'Outfit',sans-serif;background:var(--bg);color:var(--text);margin:0;padding:20px;} "
                 ".container{max-width:800px;margin:0 auto;} h1,h2,h3{font-weight:800;letter-spacing:-0.5px;margin:0 0 16px 0;} "
                 "h1{text-align:center;background:linear-gradient(135deg,#2979ff,#69f0ae);-webkit-background-clip:text;-webkit-text-fill-color:transparent;margin-bottom:30px;} "
                 "form,.card{background:var(--card-bg);backdrop-filter:blur(10px);-webkit-backdrop-filter:blur(10px);border:1px solid var(--border);border-radius:16px;padding:24px;margin-bottom:24px;box-shadow:0 8px 32px 0 rgba(0,0,0,0.3);} "
                 "input{width:100%;padding:12px;background:rgba(0,0,0,0.2);border:1px solid var(--border);border-radius:8px;color:var(--text);box-sizing:border-box;font-size:16px;margin-bottom:16px;} "
                 "button{width:100%;padding:14px;background:var(--primary);color:#fff;border:none;border-radius:8px;font-size:16px;font-weight:600;cursor:pointer;transition:opacity 0.2s;} button:active{opacity:0.8;} "
                 ".btn-danger{background:var(--accent-debt);margin-top:12px;} .customer-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:16px;border-bottom:1px solid var(--border);padding-bottom:12px;} "
                 ".badge{font-size:18px;font-weight:800;padding:6px 14px;border-radius:20px;} .badge-debt{background:rgba(255,82,82,0.15);color:var(--accent-debt);border:1px solid rgba(255,82,82,0.3);} "
                 "table{width:100%;border-collapse:collapse;font-size:14px;} th,td{padding:10px;text-align:left;} th{color:var(--text-muted);border-bottom:1px solid var(--border);} td{border-bottom:1px solid rgba(255,255,255,0.02);} "
                 ".debt-val{color:var(--accent-debt);font-weight:600;} .pay-val{color:var(--accent-pay);font-weight:600;} .item-val{color:var(--text-muted);}</style></head>"
                 "<body><div class='container'><h1>Zarah Kasuwanci</h1>");
    httpd_resp_send_chunk(req, chunk, HTTPD_RESP_USE_STRLEN);

    strcpy(chunk, "<div class='card'><h2>1. Saita WiFi (Home WiFi)</h2>"
                  "<form action='/setup' method='POST'>"
                  "<label>SSID:</label><input type='text' name='ssid' required>"
                  "<label>Password:</label><input type='password' name='pass'>"
                  "<button type='submit'>Ajiye & Sake Kunnawa</button></form></div>");
    httpd_resp_send_chunk(req, chunk, HTTPD_RESP_USE_STRLEN);

    strcpy(chunk, "<h2>2. Littafin Bashi (Debtor Ledger)</h2>");
    httpd_resp_send_chunk(req, chunk, HTTPD_RESP_USE_STRLEN);

    DIR *dir = opendir("/sdcard");
    bool found_any = false;
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            int name_len = strlen(de->d_name);
            if (name_len > 5 && strcmp(de->d_name + name_len - 5, ".json") == 0) {
                char filepath[300];
                snprintf(filepath, sizeof(filepath), "/sdcard/%s", de->d_name);

                FILE *f = fopen(filepath, "r");
                if (f) {
                    fseek(f, 0, SEEK_END);
                    long size = ftell(f);
                    fseek(f, 0, SEEK_SET);
                    char *buf = malloc(size + 1);
                    if (buf) {
                        size_t bytes_read = fread(buf, 1, size, f);
                        buf[bytes_read] = '\0';
                        cJSON *root = cJSON_Parse(buf);
                        if (root) {
                            found_any = true;
                            cJSON *name_item = cJSON_GetObjectItem(root, "name");
                            cJSON *tx_array = cJSON_GetObjectItem(root, "transactions");
                            const char *fullname = name_item ? name_item->valuestring : de->d_name;

                            // First calculate total debt
                            int total_debt = 0;
                            if (tx_array && cJSON_IsArray(tx_array)) {
                                int tx_count = cJSON_GetArraySize(tx_array);
                                for (int i = 0; i < tx_count; i++) {
                                    cJSON *tx = cJSON_GetArrayItem(tx_array, i);
                                    if (tx) {
                                        cJSON *type_item = cJSON_GetObjectItem(tx, "type");
                                        cJSON *amount_item = cJSON_GetObjectItem(tx, "amount");
                                        char type = (type_item && type_item->valuestring) ? type_item->valuestring[0] : 'T';
                                        int amt = amount_item ? amount_item->valueint : 0;
                                        if (type == 'T' || type == 'U') total_debt += amt;
                                        else if (type == 'P') total_debt -= amt;
                                    }
                                }
                            }
                            if (total_debt < 0) total_debt = 0;

                            snprintf(chunk, 4096, "<div class='card'><div class='customer-header'><h3>%s</h3>"
                                                           "<span class='badge badge-debt'>N%d</span></div>"
                                                           "<table><tr><th>Date</th><th>Item</th><th>Amount</th></tr>", fullname, total_debt);
                            httpd_resp_send_chunk(req, chunk, HTTPD_RESP_USE_STRLEN);

                            if (tx_array && cJSON_IsArray(tx_array)) {
                                int tx_count = cJSON_GetArraySize(tx_array);
                                for (int i = 0; i < tx_count; i++) {
                                    cJSON *tx = cJSON_GetArrayItem(tx_array, i);
                                    if (tx) {
                                        cJSON *ts_item = cJSON_GetObjectItem(tx, "ts");
                                        cJSON *type_item = cJSON_GetObjectItem(tx, "type");
                                        cJSON *amount_item = cJSON_GetObjectItem(tx, "amount");
                                        cJSON *item_item = cJSON_GetObjectItem(tx, "item");

                                        long ts = ts_item ? (long)ts_item->valuedouble : 0;
                                        char type = (type_item && type_item->valuestring) ? type_item->valuestring[0] : 'T';
                                        int amt = amount_item ? amount_item->valueint : 0;
                                        const char *item = (item_item && item_item->valuestring) ? item_item->valuestring : "";

                                        char date_str[32] = "-";
                                        if (ts > 10000) {
                                            struct tm tinfo;
                                            time_t time_val = (time_t)ts;
                                            localtime_r(&time_val, &tinfo);
                                            strftime(date_str, sizeof(date_str), "%d %b", &tinfo);
                                        }

                                        if (type == 'T') {
                                            snprintf(chunk, 4096, "<tr><td>%s</td><td>%s</td><td class='debt-val'>+N%d</td></tr>", date_str, item, amt);
                                        } else if (type == 'U') {
                                            snprintf(chunk, 4096, "<tr><td>%s</td><td>%s</td><td class='debt-val'>+%d pcs</td></tr>", date_str, item, amt);
                                        } else if (type == 'P') {
                                            snprintf(chunk, 4096, "<tr><td>%s</td><td><i>Biya</i></td><td class='pay-val'>-N%d</td></tr>", date_str, amt);
                                        }
                                        httpd_resp_send_chunk(req, chunk, HTTPD_RESP_USE_STRLEN);
                                    }
                                }
                            }

                            snprintf(chunk, 4096, "</table>"
                                                           "<form action='/delete' method='POST' style='background:none; margin:0; padding:0; border:none; box-shadow:none;'>"
                                                           "<input type='hidden' name='key' value='%s'>"
                                                           "<button type='submit' class='btn-danger'>Goge Abokin Hulda (Delete)</button></form></div>", de->d_name);
                            httpd_resp_send_chunk(req, chunk, HTTPD_RESP_USE_STRLEN);
                            cJSON_Delete(root);
                        }
                        free(buf);
                    }
                    fclose(f);
                }
            }
        }
        closedir(dir);
    }

    if (!found_any) {
        strcpy(chunk, "<div class='card'><p>Babu abokin hulda a littafin bashi yanzu.</p></div>");
        httpd_resp_send_chunk(req, chunk, HTTPD_RESP_USE_STRLEN);
    }
    
    strcpy(chunk, "</div></body></html>");
    httpd_resp_send_chunk(req, chunk, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    free(chunk);
    return ESP_OK;
}

static esp_err_t delete_post_handler(httpd_req_t *req) {
    char buf[128];
    int ret, remaining = req->content_len;
    if (remaining >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    if ((ret = httpd_req_recv(req, buf, remaining)) <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    
    char key[64] = {0};
    char *p_key = strstr(buf, "key=");
    if (p_key) {
        p_key += 4;
        char *end = strchr(p_key, '&');
        if (end) strncpy(key, p_key, end - p_key);
        else strcpy(key, p_key);
    }
    
    if (strlen(key) > 0) {
        char decoded_key[64];
        url_decode(decoded_key, key);
        sd_delete_customer(decoded_key);
    }
    
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t setup_post_handler(httpd_req_t *req) {
    char buf[256];
    int ret, remaining = req->content_len;
    if (remaining >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    if ((ret = httpd_req_recv(req, buf, remaining)) <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    
    char ssid_raw[32] = {0}; char pass_raw[64] = {0};
    char ssid[32] = {0};     char pass[64] = {0};
    
    char *p_ssid = strstr(buf, "ssid=");
    char *p_pass = strstr(buf, "pass=");
    if (p_ssid) {
        p_ssid += 5;
        char *end = strchr(p_ssid, '&');
        if (end) strncpy(ssid_raw, p_ssid, end - p_ssid);
        else strcpy(ssid_raw, p_ssid);
    }
    if (p_pass) {
        p_pass += 5;
        char *end = strchr(p_pass, '&');
        if (end) strncpy(pass_raw, p_pass, end - p_pass);
        else strcpy(pass_raw, p_pass);
    }
    
    url_decode(ssid, ssid_raw);
    url_decode(pass, pass_raw);
    
    if (strlen(ssid) > 0) {
        save_wifi_credentials(ssid, pass);
        httpd_resp_send(req, "<h2>WiFi Credentials Saved!</h2><p>Please turn OFF the Mode Switch and restart the device.</p>", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "Invalid SSID provided.", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

static httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.stack_size = 10240;
    
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_get = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_get);
        httpd_uri_t uri_post = { .uri = "/setup", .method = HTTP_POST, .handler = setup_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_post);
        httpd_uri_t uri_delete = { .uri = "/delete", .method = HTTP_POST, .handler = delete_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_delete);
    }
    return server;
}

static void wifi_init_softap(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "Zarah Kasuwanci Setup",
            .ssid_len = strlen("Zarah Kasuwanci Setup"),
            .channel = 1,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP started. Connect to 'Zarah Kasuwanci Setup' and go to http://192.168.4.1");
    
    lcd_update_state("WEB SETUP", RGB_BLUE, "AP: Zarah Setup", "Tafi: 192.168.4.1", "Domin saita WiFi");
}

static void wifi_init_sta(void) {
    char ssid[32] = {0};
    char pass[64] = {0};
    load_wifi_credentials(ssid, pass);

    if (strlen(ssid) == 0) {
        ESP_LOGE(TAG, "No WiFi credentials found!");
        lcd_update_state("KUSKURE", RGB_RED, "Babu bayanan WiFi!", "Saka Mode Switch", "don saita WiFi");
        while (1) {
            gpio_set_level(LED_PIN, 1); vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_PIN, 0); vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, ssid, 32);
    strncpy((char*)wifi_config.sta.password, pass, 64);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE)); 

    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

// ─── UTILITIES & LED BLINK ───────────────────────────────────────────────────

static void init_sntp(void) {
    ESP_LOGI(TAG, "Initializing SNTP");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    time_t now = 0;
    int retry = 0;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < 15) {
        ESP_LOGI(TAG, "Waiting for system time... (%d/15)", retry);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    setenv("TZ", "WAT-1", 1);
    tzset();
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    ESP_LOGI(TAG, "Current time synced: %s", asctime(&timeinfo));
}

static void hardware_monitor_task(void *arg) {
    uint32_t cooldown = 0;
    while(1) {
        if (cooldown > 0) cooldown--;
        else {
            int val = gpio_get_level(VIBRATION_SENSOR_PIN);
            if (val == 0) {  
                cooldown = 100;
                ESP_LOGI(TAG, "Vibration Alert Triggered!");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100)); 
    }
}

static void led_blink_task(void *arg) {
    int toggle = 0;
    while(1) {
        if (current_led_state == 1) { // Thinking
            gpio_set_level(LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else if (current_led_state == 2) { // Speaking
            toggle = !toggle;
            gpio_set_level(LED_PIN, toggle);
            vTaskDelay(pdMS_TO_TICKS(80));
        } else { // Idle
            gpio_set_level(LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ─── ENTRY POINT ─────────────────────────────────────────────────────────────

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_flash_init();
    }

    gpio_reset_pin(VIBRATION_SENSOR_PIN);
    gpio_set_direction(VIBRATION_SENSOR_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(VIBRATION_SENSOR_PIN, GPIO_PULLUP_ONLY);

    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 0);

    // DEVICE_PIN is disabled (-1) because GPIO 33 is used by Octal PSRAM.
    // Only initialize if a valid pin is assigned.
    if (DEVICE_PIN >= 0) {
        gpio_reset_pin(DEVICE_PIN);
        gpio_set_direction(DEVICE_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level(DEVICE_PIN, 0);
    }

    gpio_reset_pin(BUTTON_PIN);
    gpio_set_direction(BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_PIN, GPIO_PULLUP_ONLY);

    gpio_reset_pin(MODE_SWITCH_PIN);
    gpio_set_direction(MODE_SWITCH_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(MODE_SWITCH_PIN, GPIO_PULLUP_ONLY);

    vTaskDelay(pdMS_TO_TICKS(100)); // Allow pullups to settle
    int is_web_server_mode = !gpio_get_level(MODE_SWITCH_PIN); // LOW = Web Server Mode

    // Initialize LCD SPI driver first so it can show progress
    init_spi_lcd();

    // Mount SD Card
    if (init_sd_card() != ESP_OK) {
        lcd_update_state("KUSKURE", RGB_RED, "SD Card Error!", "Duba katin SD...", NULL);
        while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    if (is_web_server_mode) {
        ESP_LOGI(TAG, "Booting in Web Server (SoftAP) Mode...");
        wifi_init_softap();
        start_webserver();
        ESP_LOGI(TAG, "Web Server Mode initialized. Halting Voice Pipeline.");
    } else {
        ESP_LOGI(TAG, "Booting in Voice Assistant (STA) Mode...");
        
        lcd_update_state("BOOTING", RGB_BLUE, "Zarah v3.0 S3", "Hadin WiFi...", NULL);

        init_i2s_mic();
        init_i2s_speaker();
        play_test_tone();

        xTaskCreate(hardware_monitor_task, "hw_monitor", 2048, NULL, 5, NULL);
        xTaskCreate(led_blink_task, "led_task", 2048, NULL, 5, NULL);

        wifi_init_sta();
        init_sntp(); 

        xTaskCreatePinnedToCore(voice_pipeline_task, "pipeline_task", 16384, NULL, 5, NULL, 0);

        ESP_LOGI(TAG, "Zarah V3.0 initialized.");
    }
}
