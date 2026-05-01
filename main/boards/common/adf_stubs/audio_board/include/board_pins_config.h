#ifndef _BOARD_PINS_CONFIG_H_
#define _BOARD_PINS_CONFIG_H_

#include "driver/i2c.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "driver/spi_slave.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int mck_io_num;
    int bck_io_num;
    int ws_io_num;
    int data_out_num;
    int data_in_num;
} board_i2s_pin_t;

esp_err_t get_i2c_pins(i2c_port_t port, i2c_config_t *i2c_config);
esp_err_t get_i2s_pins(int port, board_i2s_pin_t *i2s_config);
esp_err_t get_spi_pins(spi_bus_config_t *spi_config, spi_device_interface_config_t *spi_device_interface_config);
int8_t get_sdcard_intr_gpio(void);
int8_t get_sdcard_open_file_num_max(void);
int8_t get_sdcard_power_ctrl_gpio(void);
int8_t get_auxin_detect_gpio(void);
int8_t get_headphone_detect_gpio(void);
int8_t get_pa_enable_gpio(void);
int8_t get_adc_detect_gpio(void);
int8_t get_es7243_mclk_gpio(void);
int8_t get_input_rec_id(void);
int8_t get_input_mode_id(void);
int8_t get_input_set_id(void);
int8_t get_input_play_id(void);
int8_t get_input_volup_id(void);
int8_t get_input_voldown_id(void);
int8_t get_reset_codec_gpio(void);
int8_t get_reset_board_gpio(void);
int8_t get_green_led_gpio(void);
int8_t get_blue_led_gpio(void);

#ifdef __cplusplus
}
#endif

#endif
