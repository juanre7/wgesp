/* Who is on the other side of the tunnel. WireGuard has no sessions and warns
 * about nothing: a client "connects" when its first packet arrives and
 * "disconnects" when it has been quiet for a while. Pure logic --- the clock
 * comes in as a parameter.
 */

#include "clients.h"

static struct {
    uint32_t ip;
    int64_t last_us;
} table[CLIENTS_MAX];

bool clients_seen(uint32_t src_ip, int64_t now_us)
{
    if (src_ip == 0) {
        return false; /* 0.0.0.0 marks a free slot, it cannot be a client */
    }

    int free_slot = -1;
    for (int i = 0; i < CLIENTS_MAX; i++) {
        if (table[i].ip == src_ip) {
            table[i].last_us = now_us;
            return false;
        }
        if (table[i].ip == 0 && free_slot < 0) {
            free_slot = i;
        }
    }
    /* ponytail: table full -> that client does not exist as far as the LED is
     * concerned. Eight slots are plenty for a home VPN; raise CLIENTS_MAX if
     * some day they are not. */
    if (free_slot < 0) {
        return false;
    }
    table[free_slot].ip = src_ip;
    table[free_slot].last_us = now_us;
    return true;
}

int clients_expire(int64_t now_us)
{
    int gone = 0;
    for (int i = 0; i < CLIENTS_MAX; i++) {
        if (table[i].ip && now_us - table[i].last_us > CLIENTS_IDLE_US) {
            table[i].ip = 0;
            gone++;
        }
    }
    return gone;
}

int clients_list(uint32_t *ips, int64_t *last_us, int max)
{
    int n = 0;
    for (int i = 0; i < CLIENTS_MAX && n < max; i++) {
        if (table[i].ip) {
            ips[n] = table[i].ip;
            last_us[n] = table[i].last_us;
            n++;
        }
    }
    return n;
}

