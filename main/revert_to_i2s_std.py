import os

filepath = r"C:\Users\Abdullahi Uba Aliyu\.gemini\antigravity\scratch\zarah_kasuwanci\main\main.c"

with open(filepath, 'r', encoding='utf-8') as f:
    code = f.read()

# 1. Includes
code = code.replace('#include "driver/i2s.h"', '#include "driver/i2s_std.h"')

# 2. Add globals back
old_globals = """static spi_device_handle_t spi_handle;
static volatile bool speaker_is_enabled = false;"""

new_globals = """static spi_device_handle_t spi_handle;
static i2s_chan_handle_t tx_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;
static volatile bool speaker_is_enabled = false;"""

code = code.replace(old_globals, new_globals)

# 3. Init functions
old_init = """// ─── LEGACY I2S DRIVER INTEGRATION ─────────────────────────────

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
    ESP_LOGI(TAG, "Initializing legacy I2S Speaker...");
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = 24000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 16,
        .dma_buf_len = 1024,
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
    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_1, &pin_config));
    i2s_zero_dma_buffer(I2S_NUM_1);
}

static void speaker_set_sample_rate(uint32_t rate) {
    i2s_set_sample_rates(I2S_NUM_1, rate);
}"""

new_init = """// ─── STANDARD I2S DRIVER INTEGRATION (ESP-IDF v5) ─────────────────────────────

static void init_i2s_mic(void) {
    ESP_LOGI(TAG, "Initializing standard I2S Mic...");
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL, &rx_handle));

    i2s_std_config_t rx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = -1,
            .bclk = I2S_MIC_BCLK,
            .ws = I2S_MIC_WS,
            .dout = -1,
            .din = I2S_MIC_DATA,
        },
    };
    // CRITICAL for INMP441: Even though data is 16-bit, the INMP441 expects 32-bit slots (64 clocks per frame).
    rx_std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT; 
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &rx_std_cfg));
    ESP_LOGI(TAG, "I2S Mic initialized.");
}

static void init_i2s_speaker(void) {
    ESP_LOGI(TAG, "Initializing standard I2S Speaker...");
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_handle, NULL));

    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(24000), // Default for TTS
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = -1,
            .bclk = I2S_SPK_BCLK,
            .ws = I2S_SPK_WS,
            .dout = I2S_SPK_DATA,
            .din = -1,
        },
    };
    tx_std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &tx_std_cfg));
    ESP_LOGI(TAG, "I2S Speaker initialized.");
}

static void speaker_set_sample_rate(uint32_t rate) {
    if (speaker_is_enabled) i2s_channel_disable(tx_handle);
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg);
    if (speaker_is_enabled) i2s_channel_enable(tx_handle);
}"""

code = code.replace(old_init, new_init)

# 4. record_audio
old_record_audio = """    // Flush I2S so old data doesn't build up
    size_t temp_read = 0;
    int16_t dummy[1024];
    i2s_read(I2S_NUM_0, dummy, sizeof(dummy), &temp_read, 0);"""

new_record_audio = """    i2s_channel_enable(rx_handle);
    size_t temp_read = 0;
    int16_t dummy[1024];
    i2s_channel_read(rx_handle, dummy, sizeof(dummy), &temp_read, 0);"""

code = code.replace(old_record_audio, new_record_audio)

code = code.replace('i2s_read(I2S_NUM_0, i2s_read_buff, sizeof(i2s_read_buff), &bytes_read, portMAX_DELAY);', 'i2s_channel_read(rx_handle, i2s_read_buff, sizeof(i2s_read_buff), &bytes_read, portMAX_DELAY);')

code = code.replace("""        if (gpio_get_level(BUTTON_PIN) == 1) {
            ESP_LOGI(TAG, "Button released. Stopping recording.");
            break;
        }
    }

    buf.total_samples = samples_read;""", """        if (gpio_get_level(BUTTON_PIN) == 1) {
            ESP_LOGI(TAG, "Button released. Stopping recording.");
            break;
        }
    }

    i2s_channel_disable(rx_handle);
    buf.total_samples = samples_read;""")


# 5. play_test_tone
old_test_tone = """        speaker_set_sample_rate(16000);
        for (int chunk = 0; chunk < total_samples / 1024; chunk++) {
            for (int i = 0; i < 1024; i++) {
                float time_val = (float)((chunk * 1024) + i) / sample_rate;
                sine_buf[i] = (int16_t)(sin(2.0 * M_PI * 440.0 * time_val) * 32767.0 * VOLUME_MULTIPLIER);
            }
            size_t bytes_written;
            i2s_write(I2S_NUM_1, sine_buf, 1024 * sizeof(int16_t), &bytes_written, portMAX_DELAY);
        }
        i2s_zero_dma_buffer(I2S_NUM_1);"""

new_test_tone = """        speaker_set_sample_rate(16000);
        speaker_is_enabled = true;
        i2s_channel_enable(tx_handle);
        for (int chunk = 0; chunk < total_samples / 1024; chunk++) {
            for (int i = 0; i < 1024; i++) {
                float time_val = (float)((chunk * 1024) + i) / sample_rate;
                sine_buf[i] = (int16_t)(sin(2.0 * M_PI * 440.0 * time_val) * 32767.0 * VOLUME_MULTIPLIER);
            }
            size_t bytes_written;
            i2s_channel_write(tx_handle, sine_buf, 1024 * sizeof(int16_t), &bytes_written, portMAX_DELAY);
        }
        i2s_channel_disable(tx_handle);
        speaker_is_enabled = false;"""

code = code.replace(old_test_tone, new_test_tone)


# 6. tts_player_task
old_tts = """    bool prebuffered = false;
    
    while (1) {"""

new_tts = """    bool prebuffered = false;
    
    speaker_is_enabled = true;
    i2s_channel_enable(tx_handle);
    
    while (1) {"""
code = code.replace(old_tts, new_tts)

code = code.replace('i2s_write(I2S_NUM_1, data, item_size, &bytes_written, portMAX_DELAY);', 'i2s_channel_write(tx_handle, data, item_size, &bytes_written, portMAX_DELAY);')

old_tts_end = """    i2s_zero_dma_buffer(I2S_NUM_1);
    
    tts_finished = true;"""

new_tts_end = """    i2s_channel_disable(tx_handle);
    speaker_is_enabled = false;
    
    tts_finished = true;"""

code = code.replace(old_tts_end, new_tts_end)


with open(filepath, 'w', encoding='utf-8') as f:
    f.write(code)

print("Restored std driver successfully")
