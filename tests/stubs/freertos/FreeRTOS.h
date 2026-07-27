#pragma once
#include <cstdint>
#include <cstddef>
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
#define pdPASS 1
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xFFFFFFFFU
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
