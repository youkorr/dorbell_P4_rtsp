#pragma once
#include <cstdint>
#include <cstddef>
#include "esp_err.h"
typedef void *jpeg_encoder_handle_t;
typedef enum { JPEG_ENCODE_IN_FORMAT_RGB888, JPEG_ENCODE_IN_FORMAT_RGB565,
               JPEG_ENCODE_IN_FORMAT_GRAY, JPEG_ENCODE_IN_FORMAT_YUV422 } jpeg_enc_input_format_t;
typedef enum { JPEG_DOWN_SAMPLING_YUV444, JPEG_DOWN_SAMPLING_YUV422,
               JPEG_DOWN_SAMPLING_YUV420, JPEG_DOWN_SAMPLING_GRAY } jpeg_down_sampling_type_t;
typedef enum { JPEG_ENC_ALLOC_INPUT_BUFFER, JPEG_ENC_ALLOC_OUTPUT_BUFFER } jpeg_enc_buffer_alloc_direction_t;
typedef struct { uint32_t timeout_ms; int intr_priority; } jpeg_encode_engine_cfg_t;
typedef struct { jpeg_enc_buffer_alloc_direction_t buffer_direction; } jpeg_encode_memory_alloc_cfg_t;
typedef struct { uint32_t height, width; jpeg_enc_input_format_t src_type;
                 jpeg_down_sampling_type_t sub_sample; uint32_t image_quality; } jpeg_encode_cfg_t;
esp_err_t jpeg_new_encoder_engine(const jpeg_encode_engine_cfg_t *cfg, jpeg_encoder_handle_t *h);
esp_err_t jpeg_del_encoder_engine(jpeg_encoder_handle_t h);
esp_err_t jpeg_encoder_process(jpeg_encoder_handle_t h, const jpeg_encode_cfg_t *cfg,
                               const uint8_t *src, uint32_t src_size, uint8_t *dst, uint32_t dst_size,
                               uint32_t *out_size);
void *jpeg_alloc_encoder_mem(size_t size, const jpeg_encode_memory_alloc_cfg_t *cfg, size_t *allocated);
