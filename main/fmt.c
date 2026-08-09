/* The arithmetic and the strings of the status page, with nothing from ESP-IDF
 * in them. They used to live inside status.c, where testing them meant having
 * a board plugged in; on their own they run on the PC like clients.c:
 *
 *     gcc -DFMT_SELFTEST -I main main/fmt.c -o /tmp/t && /tmp/t
 *
 * That is the whole reason this file exists. Nothing here knows about
 * handlers, sockets or sensors: it takes numbers and gives back numbers or
 * text.
 */

#include <stdio.h>
#include <string.h>

#include "fmt.h"

void fmt_ip(char *buf, uint32_t ip)
{
    snprintf(buf, FMT_IP_LEN, "%u.%u.%u.%u", (unsigned)(ip >> 24),
             (unsigned)((ip >> 16) & 0xff), (unsigned)((ip >> 8) & 0xff),
             (unsigned)(ip & 0xff));
}

void fmt_time(char *buf, long long s)
{
    if (s < 0)           snprintf(buf, FMT_TIME_LEN, "never");
    else if (s < 120)    snprintf(buf, FMT_TIME_LEN, "%lld s", s);
    else if (s < 7200)   snprintf(buf, FMT_TIME_LEN, "%lld min", s / 60);
    else if (s < 172800) snprintf(buf, FMT_TIME_LEN, "%lld h %lld min", s / 3600, (s % 3600) / 60);
    else                 snprintf(buf, FMT_TIME_LEN, "%lld d %lld h", s / 86400, (s % 86400) / 3600);
}

/* ponytail: the string is walked on every render instead of parsed at boot.
 * It is four clients and one page every 15 s; a table in RAM plus its parser
 * would be more code than this and would save nothing measurable. */
const char *client_name(char *buf, size_t len, const char *list, uint32_t ip)
{
    char key[8];
    int n = snprintf(key, sizeof(key), "%u=", (unsigned)(ip & 0xff));

    for (const char *p = list; *p; ) {
        if (strncmp(p, key, n) == 0 && (p == list || p[-1] == ',')) {
            p += n;
            size_t i = 0;
            while (p[i] && p[i] != ',' && i < len - 1) {
                buf[i] = p[i];
                i++;
            }
            buf[i] = 0;
            return i ? buf : NULL;
        }
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
    return NULL;
}

bool ip_in_net(uint32_t ip, uint32_t net, uint32_t mask)
{
    return (ip & mask) == (net & mask);
}

/* 64 bits for the product: the idle loop can run at tens of millions per
 * second and rate*100 no longer fits in 32 bits above ~42 M, which used to
 * turn the percentage into noise on the fastest samples. */
int cpu_pct_from_rate(uint32_t rate, uint32_t *max_rate)
{
    if (rate > *max_rate) {
        *max_rate = rate;
    }
    if (!*max_rate) {
        return -1; /* nothing measured yet: no idle rate to compare against */
    }
    return (int)(100 - ((uint64_t)rate * 100 / *max_rate));
}

/* GCOVR_EXCL_START --- el propio test no cuenta como codigo cubierto */
#ifdef FMT_SELFTEST
#include <assert.h>

#define EQ(a, b) assert(strcmp((a), (b)) == 0)

int main(void)
{
    char b[FMT_TIME_LEN];

    fmt_ip(b, 0x0a42420a);      EQ(b, "10.66.66.10");
    fmt_ip(b, 0);               EQ(b, "0.0.0.0");
    fmt_ip(b, 0xffffffff);      EQ(b, "255.255.255.255"); /* the longest one fits */

    fmt_time(b, -1);            EQ(b, "never");
    fmt_time(b, 0);             EQ(b, "0 s");
    fmt_time(b, 119);           EQ(b, "119 s");           /* last second in seconds */
    fmt_time(b, 120);           EQ(b, "2 min");           /* first one in minutes */
    fmt_time(b, 7199);          EQ(b, "119 min");
    fmt_time(b, 7200);          EQ(b, "2 h 0 min");
    fmt_time(b, 172799);        EQ(b, "47 h 59 min");
    fmt_time(b, 172800);        EQ(b, "2 d 0 h");
    fmt_time(b, 3 * 86400 + 4 * 3600); EQ(b, "3 d 4 h");

    const char *list = "10=phone,11=laptop,12=tv";
    char n[24];
    EQ(client_name(n, sizeof(n), list, 0x0a42420a), "phone");  /* first entry */
    EQ(client_name(n, sizeof(n), list, 0x0a42420b), "laptop"); /* middle */
    EQ(client_name(n, sizeof(n), list, 0x0a42420c), "tv");     /* last, unterminated */
    assert(!client_name(n, sizeof(n), list, 0x0a42420d));      /* not on the list */
    assert(!client_name(n, sizeof(n), "", 0x0a42420a));        /* nobody named */
    /* "1=" must not match inside "11=laptop": the key only counts after a comma */
    EQ(client_name(n, sizeof(n), "11=laptop,1=one", 0x0a424201), "one");
    assert(!client_name(n, sizeof(n), "10=", 0x0a42420a));     /* empty name is no name */
    assert(!client_name(n, sizeof(n), "10=phone,", 0x0a42420b)); /* trailing comma */
    char small[4];
    EQ(client_name(small, sizeof(small), list, 0x0a42420b), "lap"); /* truncated, not overflowed */

    assert(ip_in_net(0x0a42420a, 0x0a424201, 0xffffff00));
    assert(!ip_in_net(0x0a43420a, 0x0a424201, 0xffffff00));
    assert(ip_in_net(0x08080808, 0x0a424201, 0));  /* 0.0.0.0/0 lets everyone in */
    assert(ip_in_net(0x0a42420a, 0x0a42420a, 0xffffffff));

    uint32_t max = 0;
    assert(cpu_pct_from_rate(0, &max) == -1);       /* nothing measured yet */
    assert(cpu_pct_from_rate(1000, &max) == 0);     /* first rate calibrates: idle */
    assert(cpu_pct_from_rate(500, &max) == 50);     /* half the loops = half the CPU */
    assert(cpu_pct_from_rate(0, &max) == 100);      /* no loops at all = flat out */
    assert(cpu_pct_from_rate(2000, &max) == 0 && max == 2000); /* faster: recalibrates */
    max = 0;
    assert(cpu_pct_from_rate(100000000u, &max) == 0); /* rate*100 overflows 32 bits */
    assert(cpu_pct_from_rate(50000000u, &max) == 50);
    return 0;
}
#endif
/* GCOVR_EXCL_STOP */
