#pragma once
#include <cstddef>
void heap_caps_free(void *p);
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_SPIRAM (1 << 10)
size_t heap_caps_get_free_size(unsigned caps);
