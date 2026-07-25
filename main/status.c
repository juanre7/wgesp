/* The window into the device. A page served on the tunnel address with
 * what you need to know when the thing misbehaves and you cannot be sitting in
 * front of the serial port: CPU, heat, heap, NAPT table, clients and boots.
 *
 * Read-only on purpose: configuring over the web would require persistence,
 * validation and authentication, and would let anyone on the network brick the
 * device. Reflashing takes three minutes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "clients.h"
#include "driver/temperature_sensor.h"
#include "esp_freertos_hooks.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_netif.h"
#if CONFIG_WGESP_MDNS
#include "mdns.h"
#endif
#include "lwip/lwip_napt.h"
#include "lwip/sockets.h"
#include "lwip/stats.h"
#include "status.h"

static const char *TAG = "status";

/* Port and network allowed to look: whoever arrives through the tunnel has
 * already gone through WireGuard, and that is all the authentication needed. */
#define STATUS_PORT 80

static wireguard_ctx_t *wg;
static temperature_sensor_handle_t tsens;

/* Cumulative boots. RTC_NOINIT survives esp_restart(), the watchdog and a
 * panic, but not a power cut: unplugging = starting from scratch, which is
 * exactly what we want. The magic tells a valid count from RTC garbage. */
#define BOOTS_MAGIC 0x7e57b007u
static RTC_NOINIT_ATTR uint32_t boots_magic;
static RTC_NOINIT_ATTR uint32_t boots;

/* ---- CPU ----------------------------------------------------------------
 * ponytail: this is not a profiler, it is a rule of three. The idle hook counts
 * loops as fast as it can; utilisation is 1 - loops/loops_when_idle, and "when
 * idle" calibrates itself by keeping the best rate ever seen. It answers the
 * only question that has to be decided (60 % or 99 %?). FreeRTOS run-time stats
 * if a per-task breakdown is ever needed; they cost flash.
 * Careful: this assumes the idle task spins freely. With tickless idle or light
 * sleep the number stops meaning anything (both off today, WIFI_PS_NONE). */
static volatile uint32_t idle_loops;
static uint32_t idle_max_rate;  /* loops per second with the chip idle */
static int cpu_pct = -1;

static bool idle_hook(void)
{
    idle_loops++;
    return false; /* false = call me again right away, not once per tick */
}

void status_sample(void)
{
    static uint32_t last_loops;
    static int64_t last_us;

    uint32_t v = idle_loops;
    int64_t now = esp_timer_get_time();
    if (last_us) {
        uint32_t rate = (uint32_t)((v - last_loops) * 1000000LL / (now - last_us));
        if (rate > idle_max_rate) {
            idle_max_rate = rate;
        }
        cpu_pct = idle_max_rate ? (int)(100 - (rate * 100 / idle_max_rate)) : -1;
    }
    last_loops = v;
    last_us = now;
}

int status_cpu_pct(void)
{
    return cpu_pct;
}

float status_temp_c(void)
{
    float t;
    if (!tsens || temperature_sensor_get_celsius(tsens, &t) != ESP_OK) {
        return -1000.0f;
    }
    return t;
}

/* ---- the page ---------------------------------------------------------- */

/* buf of 16 bytes; ip in host order */
static void fmt_ip(char *buf, uint32_t ip)
{
    snprintf(buf, 16, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 0xff),
             (unsigned)((ip >> 8) & 0xff), (unsigned)(ip & 0xff));
}

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software (tunnel supervisor)";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout (bad power supply)";
    case ESP_RST_DEEPSLEEP:return "deep sleep";
    default:               return "unknown";
    }
}

/* Looks up a client name by its last octet in the CONFIG_WGESP_CLIENT_NAMES
 * list ("10=phone,11=laptop"). Returns NULL if not there.
 * ponytail: the string is walked on every render instead of parsed at boot.
 * It is four clients and one page every 15 s; a table in RAM plus its parser
 * would be more code than this and would save nothing measurable. */
static const char *client_name(char *buf, size_t len, uint32_t ip)
{
    const char *list = CONFIG_WGESP_CLIENT_NAMES;
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

/* A duration in seconds, readable: "3 d 4 h", "12 min". buf of 24. */
static void fmt_time(char *buf, long long s)
{
    if (s < 0)          snprintf(buf, 24, "never");
    else if (s < 120)   snprintf(buf, 24, "%lld s", s);
    else if (s < 7200)  snprintf(buf, 24, "%lld min", s / 60);
    else if (s < 172800)snprintf(buf, 24, "%lld h %lld min", s / 3600, (s % 3600) / 60);
    else                snprintf(buf, 24, "%lld d %lld h", s / 86400, (s % 86400) / 3600);
}

/* The real page. One-piece HTML with nothing to download: no fonts, no CSS, no
 * JS. It refreshes itself with <meta refresh>, which is what you use when the
 * alternative is putting JavaScript in a 2 MB device.
 * The plain text is still alive at /txt, for curl and for the repo scripts. */
static esp_err_t page_html(httpd_req_t *req)
{
    /* ponytail: static, not on the stack. The server task has 4 KB and with a
     * 2.5 KB buffer plus the locals it overflowed and reset the chip; it showed
     * up on the very first curl. The server is single-task, so no race. */
    static char buf[1536];
    int n = 0;
    int64_t now_us = esp_timer_get_time();

    bool up = wg && esp_wireguard_peer_is_up(wg) == ESP_OK;
    time_t hs = 0, now = 0;
    time(&now);
    if (!wg || esp_wireguard_latest_handshake(wg, &hs) != ESP_OK) {
        hs = 0;
    }
    float temp = status_temp_c();
    uint32_t heap = esp_get_free_heap_size();
    /* The percentage needs a total, and the heap total is not a known constant:
     * it depends on how much RAM startup kept (WiFi, buffers). It is computed
     * over what the heap really manages, which is what can ever be free. */
    size_t heap_total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    int heap_pct = heap_total ? (int)((uint64_t)heap * 100 / heap_total) : -1;

    char t_up[24], t_hs[24];
    fmt_time(t_up, now_us / 1000000);
    fmt_time(t_hs, hs ? (long long)(now - hs) : -1);

    /* The stylesheet is constant: it lives in flash and is sent as is, without
     * going through the buffer. That way the buffer only carries data and there
     * is no need to choose between CSS and the client list (which is exactly
     * what happened once). */
    static const char HEAD[] =
        "<!doctype html><html lang=en><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<meta http-equiv=refresh content=15>"
        "<title>wgesp</title><style>"
        ":root{--bg:#fbfbfa;--fg:#1a1a19;--dim:#71716e;--line:#e3e3e0;--ok:#1a7f4b;--no:#b3261e}"
        "@media(prefers-color-scheme:dark){:root{--bg:#161615;--fg:#ecece9;--dim:#8f8f8b;--line:#2c2c2a;--ok:#4ade80;--no:#f87171}}"
        "*{box-sizing:border-box}"
        "body{margin:0;padding:2rem 1.25rem;background:var(--bg);color:var(--fg);"
        "font:15px/1.5 ui-sans-serif,system-ui,-apple-system,Segoe UI,Roboto,sans-serif;"
        "-webkit-font-smoothing:antialiased}"
        "main{max-width:34rem;margin:0 auto}"
        "h1{font-size:1.05rem;font-weight:600;margin:0;letter-spacing:-.01em}"
        "header{display:flex;align-items:baseline;gap:.6rem;flex-wrap:wrap;"
        "padding-bottom:1rem;border-bottom:1px solid var(--line)}"
        ".sub{color:var(--dim);font-size:.85rem}"
        ".pill{margin-left:auto;font-size:.75rem;font-weight:600;letter-spacing:.04em;"
        "padding:.2rem .55rem;border-radius:99px;border:1px solid currentColor}"
        ".up{color:var(--ok)}.down{color:var(--no)}"
        "dl{display:grid;grid-template-columns:repeat(auto-fit,minmax(8rem,1fr));"
        "gap:1.25rem 1rem;margin:1.5rem 0;padding:0}"
        "dt{color:var(--dim);font-size:.75rem;text-transform:uppercase;letter-spacing:.05em}"
        "dd{margin:.15rem 0 0;font-size:1.35rem;font-variant-numeric:tabular-nums;letter-spacing:-.02em}"
        "dd small{font-size:.8rem;color:var(--dim);letter-spacing:0}"
        "h2{font-size:.75rem;text-transform:uppercase;letter-spacing:.05em;color:var(--dim);"
        "font-weight:600;margin:1.75rem 0 .5rem;padding-top:1.25rem;border-top:1px solid var(--line)}"
        "ul{list-style:none;margin:0;padding:0;font-variant-numeric:tabular-nums}"
        "li{display:flex;justify-content:space-between;gap:1rem;padding:.4rem 0;font-size:.9rem}"
        "li+li{border-top:1px solid var(--line)}"
        "li span{color:var(--dim);font-size:.8rem}"
        "li b{font-weight:600}li span:first-of-type{margin-right:auto}"
        "footer{margin-top:2rem;color:var(--dim);font-size:.75rem}"
        "a{color:inherit}";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, HEAD, HTTPD_RESP_USE_STRLEN);

    n += snprintf(buf + n, sizeof(buf) - n,
        "</style><main><header><h1>wgesp</h1>"
        "<span class=sub>ESP32-C6 &middot; %s</span>"
        "<span class='pill %s'>%s</span></header>"
        "<dl>"
        "<div><dt>Handshake</dt><dd>%s</dd></div>"
        "<div><dt>Uptime</dt><dd>%s</dd></div>"
        "<div><dt>CPU</dt><dd>%d<small> %%</small></dd></div>"
        "<div><dt>Temperature</dt><dd>%.1f<small> &deg;C</small></dd></div>"
        "<div><dt>Free memory</dt><dd>%d<small> %% &middot; %lu KB</small></dd></div>",
        CONFIG_WGESP_ADDRESS,
        up ? "up" : "down", up ? "TUNNEL UP" : "TUNNEL DOWN",
        t_hs, t_up, cpu_pct, temp > -999.0f ? temp : 0.0f,
        heap_pct, (unsigned long)(heap / 1024));

#if IP_NAPT_STATS
    unsigned long napt = lwip_stats.ip_napt.nr_active_tcp +
                         lwip_stats.ip_napt.nr_active_udp +
                         lwip_stats.ip_napt.nr_active_icmp;
    n += snprintf(buf + n, sizeof(buf) - n,
        "<div><dt>Connections</dt><dd>%lu<small> / %d</small></dd></div>",
        napt, IP_NAPT_MAX);
#endif

    uint32_t ips[CLIENTS_MAX];
    int64_t last[CLIENTS_MAX];
    int c = clients_list(ips, last, CLIENTS_MAX);
    n += snprintf(buf + n, sizeof(buf) - n, "</dl><h2>Clients (%d)</h2><ul>", c);
    if (c == 0) {
        n += snprintf(buf + n, sizeof(buf) - n, "<li>Nobody is talking through the tunnel</li>");
    }
    for (int i = 0; i < c && n < (int)sizeof(buf) - 120; i++) {
        char ip[16], seen[24], name[24];
        fmt_ip(ip, ntohl(ips[i]));
        fmt_time(seen, (now_us - last[i]) / 1000000);
        const char *who = client_name(name, sizeof(name), ntohl(ips[i]));
        if (who) {
            n += snprintf(buf + n, sizeof(buf) - n,
                          "<li><b>%s</b> <span>%s</span><span>%s ago</span></li>",
                          who, ip, seen);
        } else {
            n += snprintf(buf + n, sizeof(buf) - n,
                          "<li>%s<span>%s ago</span></li>", ip, seen);
        }
    }

    n += snprintf(buf + n, sizeof(buf) - n,
        "</ul><footer>%lu boots, the last one from %s. "
        "Raw data at <a href=/txt>/txt</a>.</footer></main>",
        (unsigned long)boots, reset_reason());

    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t page(httpd_req_t *req)
{
    char buf[1024];
    int n = 0;
    int64_t now_us = esp_timer_get_time();

    bool up = wg && esp_wireguard_peer_is_up(wg) == ESP_OK;
    time_t hs = 0, now = 0;
    time(&now);
    if (!wg || esp_wireguard_latest_handshake(wg, &hs) != ESP_OK) {
        hs = 0;
    }

    float temp = status_temp_c();
    bool has_temp = temp > -999.0f;

    n += snprintf(buf + n, sizeof(buf) - n,
                  "wgesp\n"
                  "up for %lld s, %lu boots, last one from %s\n"
                  "tunnel %s, last handshake %lld s ago\n"
                  "cpu %d %%, temperature %.1f C\n"
                  "free heap %lu B, all-time minimum %lu B\n",
                  (long long)(now_us / 1000000), (unsigned long)boots, reset_reason(),
                  up ? "UP" : "DOWN", hs ? (long long)(now - hs) : -1LL,
                  cpu_pct, has_temp ? temp : 0.0f,
                  (unsigned long)esp_get_free_heap_size(),
                  (unsigned long)esp_get_minimum_free_heap_size());

#if IP_NAPT_STATS
    /* The silent failure. If this gets close to IP_NAPT_MAX, new
     * connections start evicting old ones without saying anything. */
    n += snprintf(buf + n, sizeof(buf) - n,
                  "napt %lu/%d (tcp %lu, udp %lu, icmp %lu), %lu forced evictions\n",
                  (unsigned long)(lwip_stats.ip_napt.nr_active_tcp +
                                  lwip_stats.ip_napt.nr_active_udp +
                                  lwip_stats.ip_napt.nr_active_icmp),
                  IP_NAPT_MAX,
                  (unsigned long)lwip_stats.ip_napt.nr_active_tcp,
                  (unsigned long)lwip_stats.ip_napt.nr_active_udp,
                  (unsigned long)lwip_stats.ip_napt.nr_active_icmp,
                  (unsigned long)lwip_stats.ip_napt.nr_forced_evictions);
#endif

    uint32_t ips[CLIENTS_MAX];
    int64_t last[CLIENTS_MAX];
    int c = clients_list(ips, last, CLIENTS_MAX);
    n += snprintf(buf + n, sizeof(buf) - n, "clients %d\n", c);
    for (int i = 0; i < c && n < (int)sizeof(buf); i++) {
        char ip[16];
        fmt_ip(ip, ntohl(ips[i]));
        n += snprintf(buf + n, sizeof(buf) - n, "  %s, quiet for %lld s\n", ip,
                      (long long)((now_us - last[i]) / 1000000));
    }

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* The silent NAPT table failure. lwIP in IDF 5.5 no longer hangs
 * when the table fills up: it evicts the oldest entry and sends RST to both
 * ends (ip4_napt.c:996 and :452), so the permanent failure seen in July cannot
 * happen the same way again. What was missing was finding out: this warns in
 * the log when eviction starts or when occupancy goes over 75 %.
 * It does not reboot, on purpose: eviction is orderly degradation, and a reboot
 * would drop every connection instead of just the oldest one. */
void status_napt_check(void)
{
#if IP_NAPT_STATS
    static uint32_t last_evictions;
    uint32_t active = lwip_stats.ip_napt.nr_active_tcp +
                      lwip_stats.ip_napt.nr_active_udp +
                      lwip_stats.ip_napt.nr_active_icmp;
    uint32_t evictions = lwip_stats.ip_napt.nr_forced_evictions;

    if (evictions != last_evictions) {
        ESP_LOGW(TAG, "NAPT table full: %lu forced evictions (%lu/%d active); "
                      "old connections are being cut",
                 (unsigned long)evictions, (unsigned long)active, IP_NAPT_MAX);
        last_evictions = evictions;
    } else if (active * 4 > (uint32_t)IP_NAPT_MAX * 3) {
        ESP_LOGW(TAG, "NAPT table at %lu %% (%lu/%d active)",
                 (unsigned long)(active * 100 / IP_NAPT_MAX),
                 (unsigned long)active, IP_NAPT_MAX);
    }
#endif
}

/* ---- throughput bench ---------------------------------------------------
 * Measuring the ceiling of the device without depending on a client.
 * `/dump?mb=N` spits N MB of filler through the tunnel and, when done, leaves
 * in the log the throughput measured with the ESP clock and the CPU at the
 * time. From the VPS:
 *
 *   curl -o /dev/null http://10.66.66.6/dump?mb=50
 *
 * That path crosses the home ISP exactly once and every byte is encrypted here,
 * which is exactly what has to be measured. It is not iperf3 and does not try
 * to be: the iperf3 protocol would need a whole component for the same number.
 *
 * The server is single-task, so while the dump lasts the status page does not
 * answer: the CPU at that moment shows up in the log, at the end.
 */
#define DUMP_CHUNK 1460         /* one segment, so it is not split in two pbufs */
#define DUMP_MB_MAX 200

static esp_err_t dump(httpd_req_t *req)
{
    char q[24];
    int mb = 10;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[8];
        if (httpd_query_key_value(q, "mb", v, sizeof(v)) == ESP_OK) {
            mb = atoi(v);
        }
    }
    if (mb < 1 || mb > DUMP_MB_MAX) {
        mb = 10;
    }

    /* static: 1460 B of stack in an httpd task is not to spare, and the content
     * does not matter as long as it is not accidentally compressible downstream */
    static char chunk[DUMP_CHUNK];
    memset(chunk, 'x', sizeof(chunk));

    int chunks = (int)((int64_t)mb * 1024 * 1024 / DUMP_CHUNK);
    httpd_resp_set_type(req, "application/octet-stream");

    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < chunks; i++) {
        if (httpd_resp_send_chunk(req, chunk, sizeof(chunk)) != ESP_OK) {
            ESP_LOGW(TAG, "dump aborted at %d/%d chunks", i, chunks);
            return ESP_FAIL;    /* the client left; httpd closes the connection */
        }
    }
    int64_t us = esp_timer_get_time() - t0;
    httpd_resp_send_chunk(req, NULL, 0);

    double mbits = us ? (double)chunks * DUMP_CHUNK * 8.0 / (double)us : 0.0;
    ESP_LOGI(TAG, "dump: %d MB in %lld ms -> %.2f Mbit/s, cpu %d %%, %.1f C",
             mb, (long long)(us / 1000), mbits, cpu_pct, status_temp_c());
    return ESP_OK;
}

/* Does it come from the home LAN? It is compared against the real address and
 * netmask of the WiFi interface, not against a constant: if the home network
 * changes range tomorrow, this still holds. Only allowed when mDNS is
 * configured. With no name to resolve, the LAN door makes no sense and stays
 * shut. */
static bool from_the_lan(uint32_t ip)
{
#if !CONFIG_WGESP_MDNS
    (void)ip;
    return false;
#else
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (!sta || esp_netif_get_ip_info(sta, &info) != ESP_OK) {
        return false;
    }
    uint32_t mask = ntohl(info.netmask.addr);
    return (ip & mask) == (ntohl(info.ip.addr) & mask);
#endif
}

/* Who may look: whoever arrives through the tunnel (they already went through
 * WireGuard, and that is all the authentication needed) and, if mDNS is on,
 * whoever is on the home LAN. The IDF server can only listen on every
 * interface, so the filter goes here, when the connection is opened. */
static esp_err_t only_from_the_tunnel(httpd_handle_t hd, int sockfd)
{
    struct sockaddr_in6 addr;
    socklen_t len = sizeof(addr);
    if (getpeername(sockfd, (struct sockaddr *)&addr, &len) != 0) {
        return ESP_FAIL;
    }
    uint32_t ip = (addr.sin6_family == AF_INET6)
                      ? ntohl(((uint32_t *)&addr.sin6_addr)[3]) /* ::ffff:a.b.c.d */
                      : ntohl(((struct sockaddr_in *)&addr)->sin_addr.s_addr);
    uint32_t mask = ntohl(ipaddr_addr(CONFIG_WGESP_ALLOWED_IP_MASK));
    if ((ip & mask) != (ntohl(ipaddr_addr(CONFIG_WGESP_ALLOWED_IP)) & mask) &&
        !from_the_lan(ip)) {
        char s[16];
        fmt_ip(s, ip);
        ESP_LOGW(TAG, "connection refused from %s: neither tunnel nor LAN", s);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* mDNS has to be started BEFORE connecting the WiFi. It attaches to the
 * interface through the "got IP" event, so if it starts afterwards the event
 * has already passed and it goes deaf: it advertises internally and answers
 * nobody, neither by multicast nor by unicast. It took a while to find out
 * because mdns_init() returns ESP_OK all the same. */
void status_mdns_start(void)
{
#if CONFIG_WGESP_MDNS
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS did not start; reachable through the tunnel only");
        return;
    }
    mdns_hostname_set(CONFIG_WGESP_MDNS_HOSTNAME);
    mdns_instance_name_set("wgesp");
    mdns_service_add(NULL, "_http", "_tcp", STATUS_PORT, NULL, 0);
    ESP_LOGI(TAG, "status also at http://%s.local/ (from the LAN)",
             CONFIG_WGESP_MDNS_HOSTNAME);
#endif
}

void status_start(wireguard_ctx_t *ctx)
{
    wg = ctx;

    if (boots_magic != BOOTS_MAGIC) { /* first boot after plugging it in */
        boots_magic = BOOTS_MAGIC;
        boots = 0;
    }
    boots++;

    /* Range 20..100: under load 80.5 C were measured, right at the top of the
     * -10..80 range used before, where the reading is no longer trustworthy. */
    temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    if (temperature_sensor_install(&tcfg, &tsens) != ESP_OK ||
        temperature_sensor_enable(tsens) != ESP_OK) {
        ESP_LOGW(TAG, "no temperature sensor");
        tsens = NULL;
    }

    ESP_ERROR_CHECK(esp_register_freertos_idle_hook(idle_hook));
    status_sample(); /* first sample: utilisation shows up on the next one */

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = STATUS_PORT;
    cfg.open_fn = only_from_the_tunnel;
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "status server did not start; the device runs without it");
        return;
    }
    httpd_uri_t uri = { .uri = "/", .method = HTTP_GET, .handler = page_html };
    httpd_register_uri_handler(server, &uri);
    httpd_uri_t uri_txt = { .uri = "/txt", .method = HTTP_GET, .handler = page };
    httpd_register_uri_handler(server, &uri_txt);
    httpd_uri_t uri_dump = { .uri = "/dump", .method = HTTP_GET, .handler = dump };
    httpd_register_uri_handler(server, &uri_dump);
    ESP_LOGI(TAG, "status at http://%s/ (tunnel only)", CONFIG_WGESP_ADDRESS);
}
