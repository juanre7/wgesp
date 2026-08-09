#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define FMT_IP_LEN 16
#define FMT_TIME_LEN 24

/* An IP in host order, dotted. buf of FMT_IP_LEN. */
void fmt_ip(char *buf, uint32_t ip);

/* A duration in seconds, readable: "3 d 4 h", "12 min". Negative is "never".
 * buf of FMT_TIME_LEN. */
void fmt_time(char *buf, long long s);

/* Looks up a client name by its last octet in a "10=phone,11=laptop" list.
 * Copies it into buf (of len) and returns it; NULL if it is not there. */
const char *client_name(char *buf, size_t len, const char *list, uint32_t ip);

/* Same network? Both addresses in host order. */
bool ip_in_net(uint32_t ip, uint32_t net, uint32_t mask);

/* CPU utilisation from the idle loop rate, calibrating *max_rate with the best
 * rate ever seen. -1 while there is nothing to compare against. */
int cpu_pct_from_rate(uint32_t rate, uint32_t *max_rate);
