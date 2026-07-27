#pragma once
#include "freertos/FreeRTOS.h"
typedef void *RingbufHandle_t;
typedef enum { RINGBUF_TYPE_NOSPLIT, RINGBUF_TYPE_ALLOWSPLIT, RINGBUF_TYPE_BYTEBUF } RingbufferType_t;
RingbufHandle_t xRingbufferCreate(size_t size, RingbufferType_t type);
BaseType_t xRingbufferSend(RingbufHandle_t h, const void *data, size_t len, TickType_t wait);
void *xRingbufferReceive(RingbufHandle_t h, size_t *size, TickType_t wait);
void vRingbufferReturnItem(RingbufHandle_t h, void *item);
