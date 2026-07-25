#pragma once

#include "esp_wireguard.h"

/* Starts the window into the device: counts the boot, turns on the temperature
 * sensor, the CPU meter and the HTTP server. Call once, with the tunnel already
 * up. */
void status_start(wireguard_ctx_t *ctx);

/* Takes the CPU sample. It has to be called periodically (the supervisor in
 * main.c does it every 15 s): utilisation is measured between two calls. */
void status_sample(void);

/* CPU utilisation in % over the last window, -1 until there are two samples. */
int status_cpu_pct(void);

/* Warns in the log if the NAPT table saturates or starts evicting. Call every
 * few seconds from the supervisor. */
void status_napt_check(void);

/* Starts mDNS (http://<hostname>.local/). Call BEFORE connecting the WiFi. */
void status_mdns_start(void);

/* Chip temperature in degrees C, or -1000 if the sensor did not install. */
float status_temp_c(void);
