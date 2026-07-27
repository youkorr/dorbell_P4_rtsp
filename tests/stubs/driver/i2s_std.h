#pragma once
#include <cstdint>
#include <cstddef>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
typedef int gpio_num_t;
#define I2S_GPIO_UNUSED ((gpio_num_t) -1)
typedef enum { I2S_NUM_0 = 0, I2S_NUM_1 = 1, I2S_NUM_2 = 2 } i2s_port_t;
typedef enum { I2S_ROLE_MASTER, I2S_ROLE_SLAVE } i2s_role_t;
typedef enum { I2S_SLOT_MODE_MONO = 1, I2S_SLOT_MODE_STEREO = 2 } i2s_slot_mode_t;
typedef enum { I2S_DATA_BIT_WIDTH_16BIT = 16, I2S_DATA_BIT_WIDTH_32BIT = 32 } i2s_data_bit_width_t;
typedef enum { I2S_SLOT_BIT_WIDTH_16BIT = 16, I2S_SLOT_BIT_WIDTH_32BIT = 32 } i2s_slot_bit_width_t;
typedef enum { I2S_STD_SLOT_LEFT = 1, I2S_STD_SLOT_RIGHT = 2, I2S_STD_SLOT_BOTH = 3 } i2s_std_slot_mask_t;
typedef void *i2s_chan_handle_t;

typedef struct { i2s_port_t id; i2s_role_t role; uint32_t dma_desc_num, dma_frame_num; bool auto_clear; } i2s_chan_config_t;
typedef struct { uint32_t sample_rate_hz; int clk_src; int mclk_multiple; } i2s_std_clk_config_t;
typedef struct { i2s_data_bit_width_t data_bit_width; i2s_slot_bit_width_t slot_bit_width;
                 i2s_slot_mode_t slot_mode; i2s_std_slot_mask_t slot_mask; uint32_t ws_width;
                 bool ws_pol, bit_shift, msb_right; } i2s_std_slot_config_t;
typedef struct { struct { bool mclk_inv, bclk_inv, ws_inv; } invert_flags;
                 gpio_num_t mclk, bclk, ws, dout, din; } i2s_std_gpio_config_t_unused;
typedef struct { gpio_num_t mclk, bclk, ws, dout, din;
                 struct { bool mclk_inv, bclk_inv, ws_inv; } invert_flags; } i2s_std_gpio_config_t;
typedef struct { i2s_std_clk_config_t clk_cfg; i2s_std_slot_config_t slot_cfg; i2s_std_gpio_config_t gpio_cfg; } i2s_std_config_t;

#define I2S_CHANNEL_DEFAULT_CONFIG(port, r) { (port), (r), 6, 240, false }
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) { (uint32_t)(rate), 0, 256 }
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode) \
  { (bits), (i2s_slot_bit_width_t)(bits), (mode), I2S_STD_SLOT_BOTH, (uint32_t)(bits), true, true, false }

esp_err_t i2s_new_channel(const i2s_chan_config_t *cfg, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx);
esp_err_t i2s_del_channel(i2s_chan_handle_t h);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t h, const i2s_std_config_t *cfg);
esp_err_t i2s_channel_enable(i2s_chan_handle_t h);
esp_err_t i2s_channel_disable(i2s_chan_handle_t h);
esp_err_t i2s_channel_read(i2s_chan_handle_t h, void *dst, size_t size, size_t *read, uint32_t timeout);
esp_err_t i2s_channel_write(i2s_chan_handle_t h, const void *src, size_t size, size_t *written, uint32_t timeout);
