// Host test for core/os_idle: make -C main/core/tests/idle test
#include <stdio.h>
#include "../../os_idle.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    os_idle_t t;

    // Off never dims.
    os_idle_init(&t, 0, 0);
    CHECK(!os_idle_poll(&t, 10 * 60 * 1000));

    // Dims once, exactly at the timeout, not before.
    os_idle_init(&t, 30, 1000);
    CHECK(!os_idle_poll(&t, 30999));
    CHECK(os_idle_poll(&t, 31000));
    CHECK(!os_idle_poll(&t, 40000));        // once only
    CHECK(t.dimmed);

    // Input restores, reports it was dim, and restarts the count.
    CHECK(os_idle_input(&t, 50000));
    CHECK(!t.dimmed);
    CHECK(!os_idle_input(&t, 50100));       // not dim: nothing to restore
    CHECK(!os_idle_poll(&t, 79000));
    CHECK(os_idle_poll(&t, 80100));

    // Input before the timeout postpones it.
    os_idle_init(&t, 60, 0);
    CHECK(!os_idle_input(&t, 50000));
    CHECK(!os_idle_poll(&t, 100000));
    CHECK(os_idle_poll(&t, 110000));

    // The offered options all work; the last is 5 minutes.
    static const int opts[] = {30, 60, 120, 300};
    for (int i = 0; i < 4; i++) {
        os_idle_init(&t, opts[i], 5);
        CHECK(!os_idle_poll(&t, 5 + opts[i] * 1000LL - 1));
        CHECK(os_idle_poll(&t, 5 + opts[i] * 1000LL));
    }

    // Switching to Off while dim: input still restores, then never dims.
    os_idle_init(&t, 30, 0);
    os_idle_poll(&t, 30000);
    os_idle_set_timeout(&t, 0, 31000);
    CHECK(t.dimmed);
    CHECK(os_idle_input(&t, 32000));
    CHECK(!os_idle_poll(&t, 10000000));

    // A negative timeout (corrupt NVS) is Off.
    os_idle_init(&t, -5, 0);
    CHECK(!os_idle_poll(&t, 1000000));

    printf(fails ? "%d FAILED\n" : "idle: all tests passed\n", fails);
    return fails != 0;
}
