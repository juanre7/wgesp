/* Bench for the tunnel cipher. Compares the reference implementation shipped
 * with the library against mbedTLS and libsodium, over packets of the real size
 * WireGuard moves. Enabled with CONFIG_WGESP_CRYPTO_BENCH; it exists so that
 * decisions are made with data instead of intuition. */
#include "sdkconfig.h"

#if CONFIG_WGESP_CRYPTO_BENCH

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/chachapoly.h"

#include "crypto/refc/chacha20poly1305.h"
#include "crypto/refc/chacha20.h"
#include "crypto/refc/poly1305-donna.h"
#include "sodium.h"

static const char *TAG = "bench";

#define PKT_LEN   1400          /* typical payload inside the tunnel */
#define ITERS     200
#define TAG_LEN   16

static uint8_t buf_in[PKT_LEN];
static uint8_t buf_out[PKT_LEN + TAG_LEN];
static const uint8_t key[32] = { 0x42 };

static void report(const char *name, int64_t us)
{
    /* one pass per packet, as in real forwarding */
    double mbps = (double)ITERS * PKT_LEN * 8.0 / (double)us;
    ESP_LOGI(TAG, "%-10s %6lld us/packet  ~%.1f Mbit/s", name,
             (long long)(us / ITERS), mbps);
}

void crypto_bench(void)
{
    memset(buf_in, 0xA5, sizeof(buf_in));

    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < ITERS; i++) {
        chacha20poly1305_encrypt(buf_out, buf_in, PKT_LEN, NULL, 0, i, key);
    }
    int64_t t_refc_enc = esp_timer_get_time() - t0;

    /* With a nonce different from the one used to encrypt, the tag does not
      * match and decryption aborts before doing the work: it would measure a
      * fake figure. So we encrypt with nonce 0 and decrypt with nonce 0. */
    chacha20poly1305_encrypt(buf_out, buf_in, PKT_LEN, NULL, 0, 0, key);
    t0 = esp_timer_get_time();
    int failures = 0;
    for (int i = 0; i < ITERS; i++) {
        if (!chacha20poly1305_decrypt(buf_in, buf_out, PKT_LEN + TAG_LEN, NULL, 0, 0, key)) {
            failures++;
        }
    }
    int64_t t_refc_dec = esp_timer_get_time() - t0;
    if (failures) {
        ESP_LOGE(TAG, "%d failed decryptions: the measurement is worthless", failures);
    }

    /* The context is created per packet on purpose: that is what a drop-in
      * replacement of the macros would do, since they take the key on every
      * call. */
    mbedtls_chachapoly_context ctx;
    uint8_t nonce[12] = {0};
    uint8_t tag[TAG_LEN];

    t0 = esp_timer_get_time();
    for (int i = 0; i < ITERS; i++) {
        mbedtls_chachapoly_init(&ctx);
        mbedtls_chachapoly_setkey(&ctx, key);
        memcpy(nonce + 4, &i, sizeof(int));
        mbedtls_chachapoly_encrypt_and_tag(&ctx, PKT_LEN, nonce, NULL, 0,
                                           buf_in, buf_out, tag);
        mbedtls_chachapoly_free(&ctx);
    }
    int64_t t_mbed_enc = esp_timer_get_time() - t0;

    t0 = esp_timer_get_time();
    for (int i = 0; i < ITERS; i++) {
        mbedtls_chachapoly_init(&ctx);
        mbedtls_chachapoly_setkey(&ctx, key);
        mbedtls_chachapoly_auth_decrypt(&ctx, PKT_LEN, nonce, NULL, 0,
                                        tag, buf_out, buf_in);
        mbedtls_chachapoly_free(&ctx);
    }
    int64_t t_mbed_dec = esp_timer_get_time() - t0;

    /* And without creating the context per packet, which is the best a careful
      * integration reusing the context per keypair could aspire to. */
    mbedtls_chachapoly_init(&ctx);
    mbedtls_chachapoly_setkey(&ctx, key);
    t0 = esp_timer_get_time();
    for (int i = 0; i < ITERS; i++) {
        mbedtls_chachapoly_encrypt_and_tag(&ctx, PKT_LEN, nonce, NULL, 0,
                                           buf_in, buf_out, tag);
    }
    int64_t t_mbed_reuse = esp_timer_get_time() - t0;
    mbedtls_chachapoly_free(&ctx);

    /* Splitting the measurement. The same 1400 B, but the stream
      * cipher on one side and the authenticator on the other. The sum does not
      * add up exactly to the total (the Poly1305 key block and the padding are
      * missing), but it says which of the two is the lever. */
    struct chacha20_ctx cc;
    t0 = esp_timer_get_time();
    for (int i = 0; i < ITERS; i++) {
        chacha20_init(&cc, key, i);
        chacha20(&cc, buf_out, buf_in, PKT_LEN);
    }
    int64_t t_chacha = esp_timer_get_time() - t0;

    poly1305_context pctx;
    uint8_t mac[16];
    t0 = esp_timer_get_time();
    for (int i = 0; i < ITERS; i++) {
        poly1305_init(&pctx, key);
        poly1305_update(&pctx, buf_in, PKT_LEN);
        poly1305_finish(&pctx, mac);
    }
    int64_t t_poly = esp_timer_get_time() - t0;

    /* libsodium is already linked in (Curve25519 for the
      * handshake), so trying its AEAD does not cost a single new dependency. */
    uint8_t npub[crypto_aead_chacha20poly1305_ietf_NPUBBYTES] = {0};
    unsigned long long clen = 0;
    t0 = esp_timer_get_time();
    for (int i = 0; i < ITERS; i++) {
        memcpy(npub + 4, &i, sizeof(int));
        crypto_aead_chacha20poly1305_ietf_encrypt(buf_out, &clen, buf_in, PKT_LEN,
                                                  NULL, 0, NULL, npub, key);
    }
    int64_t t_sodium_enc = esp_timer_get_time() - t0;

    memset(npub, 0, sizeof(npub));
    crypto_aead_chacha20poly1305_ietf_encrypt(buf_out, &clen, buf_in, PKT_LEN,
                                              NULL, 0, NULL, npub, key);
    t0 = esp_timer_get_time();
    failures = 0;
    for (int i = 0; i < ITERS; i++) {
        unsigned long long mlen = 0;
        if (crypto_aead_chacha20poly1305_ietf_decrypt(buf_in, &mlen, NULL, buf_out, clen,
                                                      NULL, 0, npub, key) != 0) {
            failures++;
        }
    }
    int64_t t_sodium_dec = esp_timer_get_time() - t0;
    if (fallos) {
        ESP_LOGE(TAG, "%d failed libsodium decryptions: the measurement is worthless", failures);
    }

    ESP_LOGI(TAG, "=== ChaCha20-Poly1305, %d packets of %d bytes ===", ITERS, PKT_LEN);
    report("refc enc",  t_refc_enc);
    report("refc dec",  t_refc_dec);
    report("mbed enc",  t_mbed_enc);
    report("mbed dec",  t_mbed_dec);
    report("mbed ctx+",  t_mbed_reuse);
    report("sodium enc", t_sodium_enc);
    report("sodium dec", t_sodium_dec);
    report("chacha20",   t_chacha);
    report("poly1305",   t_poly);
    ESP_LOGI(TAG, "refc split: chacha %.0f%%, poly %.0f%% of the full AEAD",
             100.0 * t_chacha / (double)t_refc_enc,
             100.0 * t_poly / (double)t_refc_enc);
    ESP_LOGI(TAG, "against the reference one: mbedTLS x%.2f (x%.2f ctx+), libsodium x%.2f",
             (double)t_refc_enc / (double)t_mbed_enc,
             (double)t_refc_enc / (double)t_mbed_reuse,
             (double)t_refc_enc / (double)t_sodium_enc);
}

#endif /* CONFIG_WGESP_CRYPTO_BENCH */
