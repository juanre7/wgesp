#include "unity.h"
#include "clients.h"

// Note: test_clients.c must be able to clear the table to ensure independence of tests,
// but the table is static in clients.c.
// We might need to write multiple test cases or ensure the state transitions correctly
// based on the single global table.

TEST_CASE("clients seen and expire", "[clients]")
{
    const int64_t S = 1000000LL;

    // We can clear the table by expiring it at a very late time,
    // assuming no IPs were seen after that time.
    clients_expire(1000000LL * S);

    TEST_ASSERT_TRUE(clients_seen(0x0a42420a, 0));          /* new client */
    TEST_ASSERT_FALSE(clients_seen(0x0a42420a, 5 * S));     /* same one: does not count again */
    TEST_ASSERT_EQUAL(0, clients_expire(30 * S));           /* still alive */
    TEST_ASSERT_EQUAL(1, clients_expire(80 * S));           /* quiet for too long: gone */
    TEST_ASSERT_TRUE(clients_seen(0x0a42420a, 80 * S));     /* and on coming back it counts again */

    for (uint32_t i = 1; i < CLIENTS_MAX; i++) {
        TEST_ASSERT_TRUE(clients_seen(0x0a42420a + i, 80 * S));
    }
    TEST_ASSERT_FALSE(clients_seen(0xdeadbeef, 80 * S));    /* table full: neither counts nor breaks */
    TEST_ASSERT_EQUAL(CLIENTS_MAX, clients_expire(200 * S));
    TEST_ASSERT_FALSE(clients_seen(0, 200 * S));            /* 0.0.0.0 is nobody */

    uint32_t ips[CLIENTS_MAX];
    int64_t last[CLIENTS_MAX];
    TEST_ASSERT_EQUAL(0, clients_list(ips, last, CLIENTS_MAX));  /* empty table */
    TEST_ASSERT_TRUE(clients_seen(0x0a42420a, 300 * S));
    TEST_ASSERT_TRUE(clients_list(ips, last, CLIENTS_MAX) == 1 && ips[0] == 0x0a42420a);
    TEST_ASSERT_EQUAL(0, clients_list(ips, last, 0));            /* max 0 does not overflow */
}
