#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *handle, BaseType_t core);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t t);
void taskYIELD();
