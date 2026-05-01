#include "board_pins_config.h"

esp_err_t get_i2c_pins(i2c_port_t port, i2c_config_t *i2c_config) {
    return ESP_FAIL;
}

esp_err_t get_i2s_pins(int port, board_i2s_pin_t *i2s_config) {
    return ESP_FAIL;
}

esp_err_t get_spi_pins(spi_bus_config_t *spi_config, spi_device_interface_config_t *spi_device_interface_config) {
    return ESP_FAIL;
}

int8_t get_sdcard_intr_gpio(void) { return -1; }
int8_t get_sdcard_open_file_num_max(void) { return -1; }
int8_t get_sdcard_power_ctrl_gpio(void) { return -1; }
int8_t get_auxin_detect_gpio(void) { return -1; }
int8_t get_headphone_detect_gpio(void) { return -1; }
int8_t get_pa_enable_gpio(void) { return -1; }
int8_t get_adc_detect_gpio(void) { return -1; }
int8_t get_es7243_mclk_gpio(void) { return -1; }
int8_t get_input_rec_id(void) { return -1; }
int8_t get_input_mode_id(void) { return -1; }
int8_t get_input_set_id(void) { return -1; }
int8_t get_input_play_id(void) { return -1; }
int8_t get_input_volup_id(void) { return -1; }
int8_t get_input_voldown_id(void) { return -1; }
int8_t get_reset_codec_gpio(void) { return -1; }
int8_t get_reset_board_gpio(void) { return -1; }
int8_t get_green_led_gpio(void) { return -1; }
int8_t get_blue_led_gpio(void) { return -1; }
