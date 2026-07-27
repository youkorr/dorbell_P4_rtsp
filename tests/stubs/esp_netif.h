#pragma once
#include <cstdint>
#include "esp_err.h"
typedef struct esp_netif_obj esp_netif_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
esp_netif_t *esp_netif_get_default_netif();
esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *info);
#define IPSTR "%d.%d.%d.%d"
#define IP2STR(a) (int)((a)->addr & 0xff), (int)(((a)->addr >> 8) & 0xff), \
                  (int)(((a)->addr >> 16) & 0xff), (int)(((a)->addr >> 24) & 0xff)
