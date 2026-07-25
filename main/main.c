#include <string.h>
#include <time.h>

#include "clients.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wireguard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/rtc.h"
#include "hal/wdt_hal.h"
#include "lwip/ip4_addr.h"
#include "lwip/lwip_napt.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "sdkconfig.h"
#include "status.h"

static const char *TAG = "wgesp";

/* ---- a real watchdog ----------------------------------------------------
 * The tunnel supervisor lives INSIDE what it supervises: if the system jams,
 * it goes down with it. The Task WDT (5 s, with panic) covers idle starvation
 * and the Int WDT covers interrupts, but both are software on top of FreeRTOS.
 * The RTC WDT is not: it lives in the always-on domain, counts with the slow
 * clock and resets the chip no matter what the CPU is doing. It is what we
 * wanted from the LP core without writing firmware for a second
 * core or soldering a wire.
 *
 * It is armed when entering the supervisor loop, not before: startup (WiFi,
 * SNTP) has long and legitimate waits that must not trigger a reboot. From
 * then on, if the loop stops feeding it for WDT_SECONDS, the chip reboots.
 */
#define WDT_SECONDS 60

/* The bootloader leaves the RTC WDT armed at 9 s and with
 * BOOTLOADER_WDT_DISABLE_IN_USER_CODE nobody turns it off: startup (WiFi +
 * SNTP) takes ~8.5 s and the board would sometimes reboot before reaching the
 * supervisor. So the first thing app_main does is give itself some startup
 * headroom. The 5 minutes are the same as the tunnel supervisor's: if after
 * five minutes there is no WiFi, no clock and no tunnel, rebooting is exactly
 * what we want. */
#define WDT_STARTUP_SECONDS 300

static wdt_hal_context_t rtc_wdt;

static void rtc_wdt_deadline(int seconds)
{
    uint32_t ticks = (uint32_t)((uint64_t)seconds * rtc_clk_slow_freq_get_hz());
    wdt_hal_init(&rtc_wdt, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&rtc_wdt);
    wdt_hal_config_stage(&rtc_wdt, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_SYSTEM);
    wdt_hal_feed(&rtc_wdt);
    wdt_hal_enable(&rtc_wdt);
    wdt_hal_write_protect_enable(&rtc_wdt);
    ESP_LOGI(TAG, "RTC watchdog armed at %d s (independent of FreeRTOS)", seconds);
}

static void rtc_wdt_heartbeat(void)
{
    wdt_hal_write_protect_disable(&rtc_wdt);
    wdt_hal_feed(&rtc_wdt);
    wdt_hal_write_protect_enable(&rtc_wdt);
}

static wireguard_config_t wg_config = ESP_WIREGUARD_CONFIG_DEFAULT();
static wireguard_ctx_t wg_ctx = ESP_WIREGUARD_CONTEXT_DEFAULT();

/* On-board LED of the Beetle ESP32-C6 (IO15/D13). If your unit is wired the
 * other way round, set LED_ON to 0 and you are done. The other LED, the
 * charging one, belongs to the TP4057: it cannot be turned off in software
 * (see README). */
#define LED_GPIO 15
#define LED_ON 1

/* One blink at startup, two when the tunnel comes up, one long blink whenever
 * a client arrives or leaves. Blocks the caller: only used from the app_main
 * task, which is in no hurry. */
static void led_blink(int times, int ms)
{
    for (int i = 0; i < times; i++) {
        gpio_set_level(LED_GPIO, LED_ON);
        vTaskDelay(pdMS_TO_TICKS(ms));
        gpio_set_level(LED_GPIO, !LED_ON);
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

/* ponytail: the TCP/IP thread writes, the supervisor reads and clears. Aligned
 * word on a single core: the worst that can happen is one blink too many or
 * too few. An event queue if it ever matters. */
static volatile int client_events;

/* Called from the TCP/IP thread for every packet leaving the tunnel (weak hook
 * in wireguardif.c). No logging and no LED in here. */
void wgesp_client_seen(uint32_t src_ip)
{
    if (clients_seen(src_ip, esp_timer_get_time())) {
        client_events++;
    }
}

/* All of this touches the lwIP core (netif_add, udp_bind and above all the
 * sys_timeout of the WireGuard timer), which belongs exclusively to the TCP/IP
 * thread: started from another task the timer never runs, and without it there
 * are no keepalives and no key renegotiation. Hence its own context. */
static esp_err_t wg_init(void *ctx)
{
    return esp_wireguard_init(&wg_config, &wg_ctx);
}

/* Retried by the caller: with a hostname endpoint the first call usually fails
 * because DNS resolution is still in flight. Kept apart from wg_init() because
 * that one would create a new netif on every attempt. */
static esp_err_t wg_connect(void *ctx)
{
    esp_err_t err = esp_wireguard_connect(&wg_ctx);
    if (err != ESP_OK) {
        return err;
    }

    /* The tunnel network and the home LAN. Both are needed: on receive, the
     * library compares the packet DESTINATION against this list, so without
     * the LAN it drops exactly what has to be forwarded. Without this the
     * tunnel comes up but does not route; even so we do not reboot: a boot
     * loop in a device that lives plugged in is worse than a log line. */
    const char *allowed[][2] = {
        { CONFIG_WGESP_ALLOWED_IP, CONFIG_WGESP_ALLOWED_IP_MASK },
        { CONFIG_WGESP_LAN_IP,     CONFIG_WGESP_LAN_MASK },
#if CONFIG_WGESP_EXIT_NODE
        /* exit node: any destination, so clients browse with the home public
         * IP address */
        { "0.0.0.0",               "0.0.0.0" },
#endif
    };
    for (int i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        err = esp_wireguard_add_allowed_ip(&wg_ctx, allowed[i][0], allowed[i][1]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "add_allowed_ip %s/%s: %s (any slots left in WIREGUARD_MAX_SRC_IPS?)",
                     allowed[i][0], allowed[i][1], esp_err_to_name(err));
        }
    }

    /* NAPT on the wg netif (the INBOUND interface): ip_napt_forward masquerades
     * forwarded packets with the address of the outbound interface, i.e. WiFi. */
    if (ip_napt_enable_netif(wg_ctx.netif, 1) != 1) {
        ESP_LOGE(TAG, "NAPT not enabled (wg netif down): no LAN access");
    } else {
        ESP_LOGI(TAG, "NAPT active: forwarding tunnel->LAN");
    }
    return ESP_OK;
}

static void wg_start(void)
{
    wg_config.private_key = CONFIG_WGESP_PRIVATE_KEY;
    wg_config.public_key = CONFIG_WGESP_PEER_PUBLIC_KEY;
    /* the library expects NULL, not an empty string, when there is no PSK */
    wg_config.preshared_key = strlen(CONFIG_WGESP_PRESHARED_KEY) ? CONFIG_WGESP_PRESHARED_KEY : NULL;
    wg_config.endpoint = CONFIG_WGESP_ENDPOINT;
    wg_config.port = CONFIG_WGESP_ENDPOINT_PORT;
    wg_config.address = CONFIG_WGESP_ADDRESS;
    wg_config.netmask = CONFIG_WGESP_NETMASK;
    wg_config.persistent_keepalive = 25;

    ESP_ERROR_CHECK(esp_netif_tcpip_exec(wg_init, NULL));

    esp_err_t err;
    while ((err = esp_netif_tcpip_exec(wg_connect, NULL)) != ESP_OK) {
        ESP_LOGW(TAG, "connecting to %s: %s, retrying in 5s",
                 CONFIG_WGESP_ENDPOINT, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    status_start(&wg_ctx);

    /* Safety net: if the tunnel does not come back on its own, reboot. In one
     * stroke it covers the jams we cannot anticipate (new IP after a WiFi
     * reconnect, endpoint changing address, lwIP stuck). */
    const int64_t max_down_us = CONFIG_WGESP_RESTART_AFTER_MIN * 60LL * 1000000LL;
    int64_t down_since = 0;
    bool was_up = false;

    rtc_wdt_deadline(WDT_SECONDS);

    /* The loop beats once a second for the LED; the status log stays at 15. */
    for (int tick = 0;; tick++) {
        rtc_wdt_heartbeat();
        client_events += clients_expire(esp_timer_get_time());
        if (client_events) {
            ESP_LOGI(TAG, "%d client(s) coming or going: blinking", client_events);
            client_events = 0;
            led_blink(1, 1000);
        }

        if (tick % 15 == 0) {
            bool up = (esp_wireguard_peer_is_up(&wg_ctx) == ESP_OK);
            time_t hs = 0, now = 0;
            time(&now);
            if (esp_wireguard_latest_handshake(&wg_ctx, &hs) != ESP_OK) {
                hs = 0;
            }
            status_sample();
            status_napt_check();
            ESP_LOGI(TAG, "peer %s, last handshake %llds ago, free heap %lu, cpu %d%%, %.1f C",
                     up ? "UP" : "DOWN", hs ? (long long)(now - hs) : -1LL,
                     (unsigned long)esp_get_free_heap_size(), status_cpu_pct(),
                     status_temp_c());

            if (up && !was_up) {
                led_blink(2, 150); /* tunnel up: everything working */
            }
            was_up = up;

            if (up) {
                down_since = 0;
            } else if (down_since == 0) {
                down_since = esp_timer_get_time();
            } else if (max_down_us > 0 && esp_timer_get_time() - down_since > max_down_us) {
                ESP_LOGE(TAG, "tunnel down for %d min: rebooting", CONFIG_WGESP_RESTART_AFTER_MIN);
                esp_restart();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    rtc_wdt_deadline(WDT_STARTUP_SECONDS);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    led_blink(1, 150); /* I am alive */

#if CONFIG_WGESP_CRYPTO_BENCH
    extern void crypto_bench(void);
    crypto_bench();
#endif

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    status_mdns_start();   /* before connecting: it hooks onto the IP event */
    ESP_ERROR_CHECK(example_connect());

    /* it lives plugged in: power saving only adds latency and losses
     * (300 ms ping times were measured inside our own LAN) */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    /* SNTP is mandatory: the WireGuard handshake requires a synced clock */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_cfg));
    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) != ESP_OK) {
        ESP_LOGW(TAG, "waiting for SNTP sync...");
    }
    time_t now;
    time(&now);
    ESP_LOGI(TAG, "clock synced: %s", ctime(&now));

    if (strlen(CONFIG_WGESP_PRIVATE_KEY) == 0) {
        ESP_LOGW(TAG, "no WireGuard private key: wifi+sntp only");
        return;
    }
    wg_start();
}
