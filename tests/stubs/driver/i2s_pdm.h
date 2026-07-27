#pragma once
#include "driver/i2s_std.h"
typedef enum { I2S_PDM_SLOT_RIGHT = 1, I2S_PDM_SLOT_LEFT = 2, I2S_PDM_SLOT_BOTH = 3 } i2s_pdm_slot_mask_t;
typedef struct { uint32_t sample_rate_hz; int clk_src; int mclk_multiple; int dn_sample_mode; } i2s_pdm_rx_clk_config_t;
typedef struct { i2s_data_bit_width_t data_bit_width; i2s_slot_bit_width_t slot_bit_width;
                 i2s_slot_mode_t slot_mode; i2s_pdm_slot_mask_t slot_mask; } i2s_pdm_rx_slot_config_t;
typedef struct { gpio_num_t clk, din; struct { bool clk_inv; } invert_flags; } i2s_pdm_rx_gpio_config_t;
typedef struct { i2s_pdm_rx_clk_config_t clk_cfg; i2s_pdm_rx_slot_config_t slot_cfg;
                 i2s_pdm_rx_gpio_config_t gpio_cfg; } i2s_pdm_rx_config_t;
#define I2S_PDM_RX_CLK_DEFAULT_CONFIG(rate) { (uint32_t)(rate), 0, 256, 0 }
#define I2S_PDM_RX_SLOT_DEFAULT_CONFIG(bits, mode) \
  { (bits), (i2s_slot_bit_width_t)(bits), (mode), I2S_PDM_SLOT_BOTH }
esp_err_t i2s_channel_init_pdm_rx_mode(i2s_chan_handle_t h, const i2s_pdm_rx_config_t *cfg);
