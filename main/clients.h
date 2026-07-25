#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CLIENTS_MAX 8
#define CLIENTS_IDLE_US (60 * 1000000LL)

/* true the first time that IP is seen (or if it comes back after expiring) */
bool clients_seen(uint32_t src_ip, int64_t now_us);

/* forgets those quiet for CLIENTS_IDLE_US; returns how many there were */
int clients_expire(int64_t now_us);

/* copies the live ones into ips[]/last_us[] (up to max); returns how many */
int clients_list(uint32_t *ips, int64_t *last_us, int max);
