/* Who is on the other side of the tunnel. WireGuard has no sessions and warns
 * about nothing: a client "connects" when its first packet arrives and
 * "disconnects" when it has been quiet for a while. Pure logic --- the clock
 * comes in as a parameter --- so it can be tested on the PC without a board:
 *
 *     gcc -DCLIENTS_SELFTEST -I main main/clients.c -o /tmp/t && /tmp/t
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

#ifdef CLIENTS_SELFTEST
#include <assert.h>

int main(void)
{
    const int64_t S = 1000000LL;

    assert(clients_seen(0x0a42420a, 0));          /* new client */
    assert(!clients_seen(0x0a42420a, 5 * S));     /* same one: does not count again */
    assert(clients_expire(30 * S) == 0);          /* still alive */
    assert(clients_expire(5 * S + CLIENTS_IDLE_US) == 0); /* exact boundary: still alive */
    assert(clients_expire(5 * S + CLIENTS_IDLE_US + 1) == 1); /* just over boundary: gone */
    assert(clients_seen(0x0a42420a, 80 * S));     /* and on coming back it counts again */

    for (uint32_t i = 1; i < CLIENTS_MAX; i++) {
        assert(clients_seen(0x0a42420a + i, 80 * S));
    }
    assert(!clients_seen(0xdeadbeef, 80 * S));    /* table full: neither counts nor breaks */
    assert(clients_expire(200 * S) == CLIENTS_MAX);
    assert(!clients_seen(0, 200 * S));            /* 0.0.0.0 is nobody */

    uint32_t ips[CLIENTS_MAX];
    int64_t last[CLIENTS_MAX];
    assert(clients_list(ips, last, CLIENTS_MAX) == 0);  /* empty table */
    assert(clients_seen(0x0a42420a, 300 * S));
    assert(clients_list(ips, last, CLIENTS_MAX) == 1 && ips[0] == 0x0a42420a);
    assert(clients_list(ips, last, 0) == 0);            /* max 0 does not overflow */
    return 0;
}
#endif
